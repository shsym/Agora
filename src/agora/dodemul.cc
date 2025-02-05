/**
 * @file dodemul.cc
 * @brief Implmentation file for the DoDemul class.
 */
#include "dodemul.h"

#include "concurrent_queue_wrapper.h"

static constexpr bool kUseSIMDGather = true;

DoDemul::DoDemul(
    Config* config, int tid, Table<complex_float>& data_buffer,
    PtrGrid<kFrameWnd, kMaxDataSCs, complex_float>& ul_zf_matrices,
    Table<complex_float>& ue_spec_pilot_buffer,
    Table<complex_float>& equal_buffer,
    PtrCube<kFrameWnd, kMaxSymbols, kMaxUEs, int8_t>& demod_buffers,
    PhyStats* in_phy_stats, Stats* stats_manager)
    : Doer(config, tid),
      data_buffer_(data_buffer),
      ul_zf_matrices_(ul_zf_matrices),
      ue_spec_pilot_buffer_(ue_spec_pilot_buffer),
      equal_buffer_(equal_buffer),
      demod_buffers_(demod_buffers),
      phy_stats_(in_phy_stats) {
  duration_stat_ = stats_manager->GetDurationStat(DoerType::kDemul, tid);

  data_gather_buffer_ =
      static_cast<complex_float*>(Agora_memory::PaddedAlignedAlloc(
          Agora_memory::Alignment_t::kAlign64,
          kSCsPerCacheline * kMaxAntennas * sizeof(complex_float)));
  equaled_buffer_temp_ =
      static_cast<complex_float*>(Agora_memory::PaddedAlignedAlloc(
          Agora_memory::Alignment_t::kAlign64,
          cfg_->DemulBlockSize() * kMaxUEs * sizeof(complex_float)));
  equaled_buffer_temp_transposed_ =
      static_cast<complex_float*>(Agora_memory::PaddedAlignedAlloc(
          Agora_memory::Alignment_t::kAlign64,
          cfg_->DemulBlockSize() * kMaxUEs * sizeof(complex_float)));

  // phase offset calibration data
  auto* ue_pilot_ptr =
      reinterpret_cast<arma::cx_float*>(cfg_->UeSpecificPilot()[0]);
  arma::cx_fmat mat_pilot_data(ue_pilot_ptr, cfg_->OfdmDataNum(),
                               cfg_->UeAntNum(), false);
  ue_pilot_data_ = mat_pilot_data.st();

#if USE_MKL_JIT
  MKL_Complex8 alpha = {1, 0};
  MKL_Complex8 beta = {0, 0};

  mkl_jit_status_t status = mkl_jit_create_cgemm(
      &jitter_, MKL_COL_MAJOR, MKL_NOTRANS, MKL_NOTRANS, cfg_->UeNum(), 1,
      cfg_->BsAntNum(), &alpha, cfg_->UeNum(), cfg_->BsAntNum(), &beta,
      cfg_->UeNum());
  if (MKL_JIT_ERROR == status) {
    std::fprintf(
        stderr,
        "Error: insufficient memory to JIT and store the DGEMM kernel\n");
    throw std::runtime_error(
        "DoDemul: insufficient memory to JIT and store the DGEMM kernel");
  }
  mkl_jit_cgemm_ = mkl_jit_get_cgemm_ptr(jitter_);
#endif
}

DoDemul::~DoDemul() {
  std::free(data_gather_buffer_);
  std::free(equaled_buffer_temp_);
  std::free(equaled_buffer_temp_transposed_);

#if USE_MKL_JIT
  mkl_jit_status_t status = mkl_jit_destroy(jitter_);
  if (MKL_JIT_ERROR == status) {
    std::fprintf(stderr, "!!!!Error: Error while destorying MKL JIT\n");
  }
#endif
}

EventData DoDemul::Launch(size_t tag) {
  const size_t frame_id = gen_tag_t(tag).frame_id_;
  const size_t symbol_id = gen_tag_t(tag).symbol_id_;
  const size_t base_sc_id = gen_tag_t(tag).sc_id_;

  const size_t symbol_idx_ul = this->cfg_->Frame().GetULSymbolIdx(symbol_id);
  const size_t total_data_symbol_idx_ul =
      cfg_->GetTotalDataSymbolIdxUl(frame_id, symbol_idx_ul);
  const complex_float* data_buf = data_buffer_[total_data_symbol_idx_ul];

  const size_t frame_slot = frame_id % kFrameWnd;
  size_t start_tsc = GetTime::WorkerRdtsc();

  if (kDebugPrintInTask == true) {
    std::printf(
        "In doDemul tid %d: frame: %zu, symbol idx: %zu, symbol idx ul: %zu, "
        "subcarrier: %zu, databuffer idx %zu \n",
        tid_, frame_id, symbol_id, symbol_idx_ul, base_sc_id,
        total_data_symbol_idx_ul);
  }

  size_t max_sc_ite =
      std::min(cfg_->DemulBlockSize(), cfg_->OfdmDataNum() - base_sc_id);
  assert(max_sc_ite % kSCsPerCacheline == 0);
  // Iterate through cache lines
  for (size_t i = 0; i < max_sc_ite; i += kSCsPerCacheline) {
    size_t start_tsc0 = GetTime::WorkerRdtsc();

    // Step 1: Populate data_gather_buffer as a row-major matrix with
    // kSCsPerCacheline rows and BsAntNum() columns

    // Since kSCsPerCacheline divides demul_block_size and
    // kTransposeBlockSize, all subcarriers (base_sc_id + i) lie in the
    // same partial transpose block.
    const size_t partial_transpose_block_base =
        ((base_sc_id + i) / kTransposeBlockSize) *
        (kTransposeBlockSize * cfg_->BsAntNum());

    size_t ant_start = 0;
    if (kUseSIMDGather and cfg_->BsAntNum() % 4 == 0 and kUsePartialTrans) {
      // Gather data for all antennas and 8 subcarriers in the same cache
      // line, 1 subcarrier and 4 (AVX2) or 8 (AVX512) ants per iteration
      size_t cur_sc_offset =
          partial_transpose_block_base + (base_sc_id + i) % kTransposeBlockSize;
      const auto* src = (const float*)&data_buf[cur_sc_offset];
      auto* dst = (float*)data_gather_buffer_;
#ifdef __AVX512F__
      size_t ant_num_per_simd = 8;
      __m512i index = _mm512_setr_epi32(
          0, 1, kTransposeBlockSize * 2, kTransposeBlockSize * 2 + 1,
          kTransposeBlockSize * 4, kTransposeBlockSize * 4 + 1,
          kTransposeBlockSize * 6, kTransposeBlockSize * 6 + 1,
          kTransposeBlockSize * 8, kTransposeBlockSize * 8 + 1,
          kTransposeBlockSize * 10, kTransposeBlockSize * 10 + 1,
          kTransposeBlockSize * 12, kTransposeBlockSize * 12 + 1,
          kTransposeBlockSize * 14, kTransposeBlockSize * 14 + 1);
      for (size_t ant_i = 0; ant_i < cfg_->BsAntNum();
           ant_i += ant_num_per_simd) {
        for (size_t j = 0; j < kSCsPerCacheline; j++) {
          __m512 data_rx = kTransposeBlockSize == 1
                               ? _mm512_load_ps(src + j * cfg_->BsAntNum() * 2)
                               : _mm512_i32gather_ps(index, src + j * 2, 4);
          _mm512_store_ps(dst + j * cfg_->BsAntNum() * 2, data_rx);
        }
        src += ant_num_per_simd * kTransposeBlockSize * 2;
        dst += ant_num_per_simd * 2;
      }
#else
      size_t ant_num_per_simd = 4;
      __m256i index = _mm256_setr_epi32(
          0, 1, kTransposeBlockSize * 2, kTransposeBlockSize * 2 + 1,
          kTransposeBlockSize * 4, kTransposeBlockSize * 4 + 1,
          kTransposeBlockSize * 6, kTransposeBlockSize * 6 + 1);
      for (size_t ant_i = 0; ant_i < cfg_->BsAntNum();
           ant_i += ant_num_per_simd) {
        for (size_t j = 0; j < kSCsPerCacheline; j++) {
          __m256 data_rx = _mm256_i32gather_ps(src + j * 2, index, 4);
          _mm256_store_ps(dst + j * cfg_->BsAntNum() * 2, data_rx);
        }
        src += ant_num_per_simd * kTransposeBlockSize * 2;
        dst += ant_num_per_simd * 2;
      }
#endif
      // Set the remaining number of antennas for non-SIMD gather
      ant_start = cfg_->BsAntNum() % 4;
    } else {
      complex_float* dst = data_gather_buffer_ + ant_start;
      for (size_t j = 0; j < kSCsPerCacheline; j++) {
        for (size_t ant_i = ant_start; ant_i < cfg_->BsAntNum(); ant_i++) {
          *dst++ =
              kUsePartialTrans
                  ? data_buf[partial_transpose_block_base +
                             (ant_i * kTransposeBlockSize) +
                             ((base_sc_id + i + j) % kTransposeBlockSize)]
                  : data_buf[ant_i * cfg_->OfdmDataNum() + base_sc_id + i + j];
        }
      }
    }
    duration_stat_->task_duration_[1] += GetTime::WorkerRdtsc() - start_tsc0;

    // Step 2: For each subcarrier, perform equalization by multiplying the
    // subcarrier's data from each antenna with the subcarrier's precoder
    for (size_t j = 0; j < kSCsPerCacheline; j++) {
      const size_t cur_sc_id = base_sc_id + i + j;

      arma::cx_float* equal_ptr = nullptr;
      if (kExportConstellation) {
        equal_ptr =
            (arma::cx_float*)(&equal_buffer_[total_data_symbol_idx_ul]
                                            [cur_sc_id * cfg_->UeNum()]);
      } else {
        equal_ptr =
            (arma::cx_float*)(&equaled_buffer_temp_[(cur_sc_id - base_sc_id) *
                                                    cfg_->UeNum()]);
      }
      arma::cx_fmat mat_equaled(equal_ptr, cfg_->UeNum(), 1, false);

      auto* data_ptr = reinterpret_cast<arma::cx_float*>(
          &data_gather_buffer_[j * cfg_->BsAntNum()]);
      // size_t start_tsc2 = worker_rdtsc();
      auto* ul_zf_ptr = reinterpret_cast<arma::cx_float*>(
          ul_zf_matrices_[frame_slot][cfg_->GetZfScId(cur_sc_id)]);

      size_t start_tsc2 = GetTime::WorkerRdtsc();
#if USE_MKL_JIT
      mkl_jit_cgemm_(jitter_, (MKL_Complex8*)ul_zf_ptr, (MKL_Complex8*)data_ptr,
                     (MKL_Complex8*)equal_ptr);
#else
      arma::cx_fmat mat_data(data_ptr, cfg_->BsAntNum(), 1, false);

      arma::cx_fmat mat_ul_zf(ul_zf_ptr, cfg_->UeNum(), cfg_->BsAntNum(),
                              false);
      mat_equaled = mat_ul_zf * mat_data;
#endif

      if (symbol_idx_ul <
          cfg_->Frame().ClientUlPilotSymbols()) {  // Calc new phase shift
        if (symbol_idx_ul == 0 && cur_sc_id == 0) {
          // Reset previous frame
          auto* phase_shift_ptr = reinterpret_cast<arma::cx_float*>(
              ue_spec_pilot_buffer_[(frame_id - 1) % kFrameWnd]);
          arma::cx_fmat mat_phase_shift(phase_shift_ptr, cfg_->UeNum(),
                                        cfg_->Frame().ClientUlPilotSymbols(),
                                        false);
          mat_phase_shift.fill(0);
        }
        auto* phase_shift_ptr = reinterpret_cast<arma::cx_float*>(
            &ue_spec_pilot_buffer_[frame_id % kFrameWnd]
                                  [symbol_idx_ul * cfg_->UeNum()]);
        arma::cx_fmat mat_phase_shift(phase_shift_ptr, cfg_->UeNum(), 1, false);
        arma::cx_fmat shift_sc =
            sign(mat_equaled % conj(ue_pilot_data_.col(cur_sc_id)));
        mat_phase_shift += shift_sc;
      }
      // apply previously calc'ed phase shift to data
      else if (cfg_->Frame().ClientUlPilotSymbols() > 0) {
        auto* pilot_corr_ptr = reinterpret_cast<arma::cx_float*>(
            ue_spec_pilot_buffer_[frame_id % kFrameWnd]);
        arma::cx_fmat pilot_corr_mat(pilot_corr_ptr, cfg_->UeNum(),
                                     cfg_->Frame().ClientUlPilotSymbols(),
                                     false);
        arma::fmat theta_mat = arg(pilot_corr_mat);
        arma::fmat theta_inc = arma::zeros<arma::fmat>(cfg_->UeNum(), 1);
        for (size_t s = 1; s < cfg_->Frame().ClientUlPilotSymbols(); s++) {
          arma::fmat theta_diff = theta_mat.col(s) - theta_mat.col(s - 1);
          theta_inc += theta_diff;
        }
        theta_inc /= (float)std::max(
            1, static_cast<int>(cfg_->Frame().ClientUlPilotSymbols() - 1));
        arma::fmat cur_theta = theta_mat.col(0) + (symbol_idx_ul * theta_inc);
        arma::cx_fmat mat_phase_correct =
            arma::zeros<arma::cx_fmat>(size(cur_theta));
        mat_phase_correct.set_real(cos(-cur_theta));
        mat_phase_correct.set_imag(sin(-cur_theta));
        mat_equaled %= mat_phase_correct;

        // Measure EVM from ground truth
        if (symbol_idx_ul == cfg_->Frame().ClientUlPilotSymbols()) {
          phy_stats_->UpdateEvmStats(frame_id, cur_sc_id, mat_equaled);
          if (kPrintPhyStats && cur_sc_id == 0) {
            phy_stats_->PrintEvmStats(frame_id - 1);
          }
        }
      }
      size_t start_tsc3 = GetTime::WorkerRdtsc();
      duration_stat_->task_duration_[2] += start_tsc3 - start_tsc2;
      duration_stat_->task_count_++;
    }
  }

  size_t start_tsc3 = GetTime::WorkerRdtsc();
  __m256i index2 = _mm256_setr_epi32(
      0, 1, cfg_->UeNum() * 2, cfg_->UeNum() * 2 + 1, cfg_->UeNum() * 4,
      cfg_->UeNum() * 4 + 1, cfg_->UeNum() * 6, cfg_->UeNum() * 6 + 1);
  auto* equal_t_ptr = reinterpret_cast<float*>(equaled_buffer_temp_transposed_);
  for (size_t i = 0; i < cfg_->UeNum(); i++) {
    float* equal_ptr = nullptr;
    if (kExportConstellation) {
      equal_ptr = reinterpret_cast<float*>(
          &equal_buffer_[total_data_symbol_idx_ul]
                        [base_sc_id * cfg_->UeNum() + i]);
    } else {
      equal_ptr = reinterpret_cast<float*>(equaled_buffer_temp_ + i);
    }
    size_t k_num_double_in_sim_d256 = sizeof(__m256) / sizeof(double);  // == 4
    for (size_t j = 0; j < max_sc_ite / k_num_double_in_sim_d256; j++) {
      __m256 equal_t_temp = _mm256_i32gather_ps(equal_ptr, index2, 4);
      _mm256_store_ps(equal_t_ptr, equal_t_temp);
      equal_t_ptr += 8;
      equal_ptr += cfg_->UeNum() * k_num_double_in_sim_d256 * 2;
    }
    equal_t_ptr = (float*)(equaled_buffer_temp_transposed_);
    int8_t* demod_ptr = demod_buffers_[frame_slot][symbol_idx_ul][i] +
                        (cfg_->ModOrderBits() * base_sc_id);

    switch (cfg_->ModOrderBits()) {
      case (CommsLib::kQpsk):
        DemodQpskSoftSse(equal_t_ptr, demod_ptr, max_sc_ite);
        break;
      case (CommsLib::kQaM16):
        Demod16qamSoftAvx2(equal_t_ptr, demod_ptr, max_sc_ite);
        break;
      case (CommsLib::kQaM64):
        Demod64qamSoftAvx2(equal_t_ptr, demod_ptr, max_sc_ite);
        break;
      default:
        std::printf("Demodulation: modulation type %s not supported!\n",
                    cfg_->Modulation().c_str());
    }
    // std::printf("In doDemul thread %d: frame: %d, symbol: %d, sc_id: %d \n",
    //     tid, frame_id, symbol_idx_ul, base_sc_id);
    // cout << "Demuled data : \n ";
    // cout << " UE " << i << ": ";
    // for (int k = 0; k < max_sc_ite * cfg->ModOrderBits(); k++)
    //     std::printf("%i ", demul_ptr[k]);
    // cout << endl;
  }

  duration_stat_->task_duration_[3] += GetTime::WorkerRdtsc() - start_tsc3;
  duration_stat_->task_duration_[0] += GetTime::WorkerRdtsc() - start_tsc;
  return EventData(EventType::kDemul, tag);
}

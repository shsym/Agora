/**
 * @file ue_worker.cc
 * @brief Implementation file for the ue worker class.  Provides storage for
 * each worker.
 */
#include "ue_worker.h"

#include <memory>

#include "datatype_conversion.h"
#include "phy_ldpc_decoder_5gnr.h"
#include "phy_stats.h"
#include "scrambler.h"
#include "utils_ldpc.h"

/* Print debug work */
static constexpr bool kDebugPrintFft = false;
static constexpr bool kDebugPrintDemul = false;
static constexpr bool kDebugPrintModul = false;
static constexpr bool kDebugPrintDecode = false;

static constexpr bool kPrintLLRData = false;
static constexpr bool kPrintDownlinkPilotStats = false;
static constexpr bool kPrintEqualizedSymbols = false;
static constexpr size_t kRecordFrameIndex = 1000;

UeWorker::UeWorker(
    size_t tid, Config& config, Stats& shared_stats, PhyStats& shared_phy_stats,
    moodycamel::ConcurrentQueue<EventData>& notify_queue,
    moodycamel::ConcurrentQueue<EventData>& work_queue,
    moodycamel::ProducerToken& work_producer, Table<int8_t>& ul_bits_buffer,
    Table<int8_t>& encoded_buffer, Table<complex_float>& modul_buffer,
    Table<complex_float>& ifft_buffer, char* const tx_buffer,
    Table<char>& rx_buffer, std::vector<myVec>& csi_buffer,
    std::vector<myVec>& equal_buffer, std::vector<size_t>& non_null_sc_ind,
    Table<complex_float>& fft_buffer,
    PtrCube<kFrameWnd, kMaxSymbols, kMaxUEs, int8_t>& demod_buffer,
    PtrCube<kFrameWnd, kMaxSymbols, kMaxUEs, int8_t>& decoded_buffer,
    std::vector<std::vector<std::complex<float>>>& ue_pilot_vec)
    : tid_(tid),
      thread_(),
      notify_queue_(notify_queue),
      work_queue_(work_queue),
      work_producer_token_(work_producer),
      config_(config),
      stats_(shared_stats),
      phy_stats_(shared_phy_stats),
      ul_bits_buffer_(ul_bits_buffer),
      encoded_buffer_(encoded_buffer),
      modul_buffer_(modul_buffer),
      ifft_buffer_(ifft_buffer),
      tx_buffer_(tx_buffer),
      rx_buffer_(rx_buffer),
      csi_buffer_(csi_buffer),
      equal_buffer_(equal_buffer),
      non_null_sc_ind_(non_null_sc_ind),
      fft_buffer_(fft_buffer),
      demod_buffer_(demod_buffer),
      decoded_buffer_(decoded_buffer),
      ue_pilot_vec_(ue_pilot_vec) {
  ptok_ = std::make_unique<moodycamel::ProducerToken>(notify_queue);

  AllocBuffer1d(&rx_samps_tmp_, config_.SampsPerSymbol(),
                Agora_memory::Alignment_t::kAlign64, 1);

  (void)DftiCreateDescriptor(&mkl_handle_, DFTI_SINGLE, DFTI_COMPLEX, 1,
                             config_.OfdmCaNum());
  (void)DftiCommitDescriptor(mkl_handle_);
}

UeWorker::~UeWorker() {
  DftiFreeDescriptor(&mkl_handle_);
  FreeBuffer1d(&rx_samps_tmp_);
  std::printf("UeWorker[%zu] Terminated\n", tid_);
}

void UeWorker::Start(size_t core_offset) {
  if (thread_.joinable() == false) {
    thread_ = std::thread(&UeWorker::TaskThread, this, core_offset);
  } else {
    throw std::runtime_error(
        "Starting UeWorker thread when one already exists");
  }
}

void UeWorker::Stop() {
  std::printf("Joining PhyUe worker %zu\n", tid_);
  thread_.join();
}

void UeWorker::TaskThread(size_t core_offset) {
  std::printf("UeWorker[%zu]: started\n", tid_);
  PinToCoreWithOffset(ThreadType::kWorker, core_offset, tid_);

  auto encoder = std::make_unique<DoEncode>(
      &config_, (int)tid_,
      (kEnableMac == true) ? ul_bits_buffer_ : config_.UlBits(),
      (kEnableMac == true) ? kFrameWnd : 1, encoded_buffer_, &stats_);

  auto iffter = std::make_unique<DoIFFTClient>(
      &config_, (int)tid_, ifft_buffer_, tx_buffer_, &stats_);

  auto decoder =
      std::make_unique<DoDecodeClient>(&config_, (int)tid_, demod_buffer_,
                                       decoded_buffer_, &phy_stats_, &stats_);

  EventData event;
  while (config_.Running() == true) {
    if (work_queue_.try_dequeue_from_producer(work_producer_token_, event) ==
        true) {
      switch (event.event_type_) {
        case EventType::kDecode: {
          DoDecodeUe(decoder.get(), event.tags_[0]);
        } break;
        case EventType::kDemul: {
          DoDemul(event.tags_[0]);
        } break;
        case EventType::kIFFT: {
          // DoIfftUe(iffter.get(), event.tags_[0]);
          DoIfft(event.tags_[0]);
        } break;
        case EventType::kEncode: {
          DoEncodeUe(encoder.get(), event.tags_[0]);
        } break;
        case EventType::kModul: {
          DoModul(event.tags_[0]);
        } break;
        case EventType::kFFTPilot: {
          DoFftPilot(event.tags_[0]);
        } break;
        case EventType::kFFT: {
          DoFftData(event.tags_[0]);
        } break;
        default: {
          std::printf("***** Invalid Event Type [%d] in Work Queue\n",
                      static_cast<int>(event.event_type_));
        }
      }
    }  // end dequeue
  }
}

//////////////////////////////////////////////////////////
//                   DOWNLINK Operations                //
//////////////////////////////////////////////////////////
void UeWorker::DoFftData(size_t tag) {
  size_t start_tsc = GetTime::Rdtsc();

  // read info of one frame
  Packet* pkt = fft_req_tag_t(tag).rx_packet_->RawPacket();

  size_t frame_id = pkt->frame_id_;
  size_t symbol_id = pkt->symbol_id_;
  size_t ant_id = pkt->ant_id_;
  size_t frame_slot = frame_id % kFrameWnd;

  if (kDebugPrintInTask || kDebugPrintFft) {
    std::printf("UeWorker[%zu]: Fft Data(frame %zu, symbol %zu, ant %zu)\n",
                tid_, frame_id, symbol_id, ant_id);
  }

  size_t sig_offset = config_.OfdmRxZeroPrefixClient();
  if (kPrintDownlinkPilotStats) {
    if (frame_id == kRecordFrameIndex) {
      std::string fname = "rxdata" + std::to_string(symbol_id) + "_" +
                          std::to_string(ant_id) + ".bin";
      FILE* f = std::fopen(fname.c_str(), "wb");
      std::fwrite(pkt->data_, 2 * sizeof(int16_t), config_.SampsPerSymbol(), f);
      std::fclose(f);
    }
  }

  // remove CP, do FFT
  size_t dl_symbol_id = config_.Frame().GetDLSymbolIdx(symbol_id);

  size_t total_dl_symbol_id =
      (frame_slot * config_.Frame().NumDLSyms()) + dl_symbol_id;
  size_t fft_buffer_target_id =
      (total_dl_symbol_id * config_.UeAntNum()) + ant_id;

  // transfer ushort to float
  size_t delay_offset = (sig_offset + config_.CpLen()) * 2;
  auto* fft_buff = reinterpret_cast<float*>(fft_buffer_[fft_buffer_target_id]);

  SimdConvertShortToFloat(&pkt->data_[delay_offset], fft_buff,
                          config_.OfdmCaNum() * 2);

  // perform fft
  DftiComputeForward(mkl_handle_, fft_buffer_[fft_buffer_target_id]);

  size_t csi_offset = frame_slot * config_.UeAntNum() + ant_id;
  auto* csi_buffer_ptr =
      reinterpret_cast<arma::cx_float*>(csi_buffer_.at(csi_offset).data());
  auto* fft_buffer_ptr =
      reinterpret_cast<arma::cx_float*>(fft_buffer_[fft_buffer_target_id]);

  size_t dl_data_symbol_perframe = config_.Frame().NumDlDataSyms();
  size_t total_dl_data_symbol_id =
      (frame_slot * dl_data_symbol_perframe) +
      (dl_symbol_id - config_.Frame().ClientDlPilotSymbols());
  size_t eq_buffer_offset =
      total_dl_data_symbol_id * config_.UeAntNum() + ant_id;

  auto* equ_buffer_ptr = reinterpret_cast<arma::cx_float*>(
      equal_buffer_.at(eq_buffer_offset).data());

  // use pilot subcarriers for phase tracking and correction
  float theta = 0;
  for (size_t j = 0; j < config_.OfdmDataNum(); j++) {
    if (j % config_.OfdmPilotSpacing() == 0) {
      equ_buffer_ptr[j] = 0;
      size_t sc_id = non_null_sc_ind_[j];
      arma::cx_float y = fft_buffer_ptr[sc_id];
      auto pilot_eq = y / csi_buffer_ptr[j];
      auto p = config_.UeSpecificPilot()[ant_id][j];
      theta += arg(pilot_eq * arma::cx_float(p.re, -p.im));
    }
  }
  if (config_.GetOFDMPilotNum() > 0) {
    theta /= config_.GetOFDMPilotNum();
  }
  auto phc = exp(arma::cx_float(0, -theta));
  float evm = 0;
  for (size_t j = 0; j < config_.OfdmDataNum(); j++) {
    if (j % config_.OfdmPilotSpacing() != 0) {
      // divide fft output by pilot data to get CSI estimation
      size_t sc_id = non_null_sc_ind_[j];
      arma::cx_float y = fft_buffer_ptr[sc_id];
      equ_buffer_ptr[j] = (y / csi_buffer_ptr[j]) * phc;
      complex_float tx =
          config_.DlIqF()[dl_symbol_id][ant_id * config_.OfdmCaNum() +
                                        config_.OfdmDataStart() + j];
      evm += std::norm(equ_buffer_ptr[j] - arma::cx_float(tx.re, tx.im));
    }
  }

  if (kPrintEqualizedSymbols) {
    complex_float* tx =
        &config_.DlIqF()[dl_symbol_id][ant_id * config_.OfdmCaNum() +
                                       config_.OfdmDataStart()];
    arma::cx_fvec x_vec(reinterpret_cast<arma::cx_float*>(tx),
                        config_.OfdmDataNum(), false);
    Utils::PrintVec(x_vec, std::string("x") +
                               std::to_string(total_dl_symbol_id) +
                               std::string("_") + std::to_string(ant_id));
    arma::cx_fvec equal_vec(equ_buffer_ptr, config_.OfdmDataNum(), false);
    Utils::PrintVec(equal_vec, std::string("equ") +
                                   std::to_string(total_dl_symbol_id) +
                                   std::string("_") + std::to_string(ant_id));
  }
  evm = std::sqrt(evm) / (config_.OfdmDataNum() - config_.GetOFDMPilotNum());
  if (kPrintPhyStats) {
    std::stringstream ss;
    ss << "Frame: " << frame_id << ", Symbol: " << symbol_id
       << ", User: " << ant_id << ", EVM: " << 100 * evm
       << "%, SNR: " << -10 * std::log10(evm) << std::endl;
    std::cout << ss.str();
  }

  if (kDebugPrintPerTaskDone || kDebugPrintFft) {
    size_t fft_duration_stat = GetTime::Rdtsc() - start_tsc;
    std::printf(
        "UeWorker[%zu]: Fft Data(frame %zu, symbol %zu, ant %zu) Duration "
        "%2.4f ms\n",
        tid_, frame_id, symbol_id, ant_id,
        GetTime::CyclesToMs(fft_duration_stat, GetTime::MeasureRdtscFreq()));
  }

  // Free the rx buffer
  fft_req_tag_t(tag).rx_packet_->Free();

  EventData fft_finish_event = EventData(
      EventType::kFFT, gen_tag_t::FrmSymAnt(frame_id, symbol_id, ant_id).tag_);
  RtAssert(notify_queue_.enqueue(*ptok_.get(), fft_finish_event),
           "UeWorker: FFT message enqueue failed");
}

void UeWorker::DoFftPilot(size_t tag) {
  size_t start_tsc = GetTime::Rdtsc();

  // read info of one frame
  Packet* pkt = fft_req_tag_t(tag).rx_packet_->RawPacket();

  size_t frame_id = pkt->frame_id_;
  size_t symbol_id = pkt->symbol_id_;
  size_t ant_id = pkt->ant_id_;
  size_t frame_slot = frame_id % kFrameWnd;

  if (kDebugPrintInTask || kDebugPrintFft) {
    std::printf("UeWorker[%zu]: Fft Pilot(frame %zu, symbol %zu, ant %zu)\n",
                tid_, frame_id, symbol_id, ant_id);
  }

  size_t sig_offset = config_.OfdmRxZeroPrefixClient();
  if (kPrintDownlinkPilotStats) {
    SimdConvertShortToFloat(pkt->data_, reinterpret_cast<float*>(rx_samps_tmp_),
                            2 * config_.SampsPerSymbol());
    std::vector<std::complex<float>> samples_vec(
        rx_samps_tmp_, rx_samps_tmp_ + config_.SampsPerSymbol());
    size_t seq_len = ue_pilot_vec_[ant_id].size();
    std::vector<std::complex<float>> pilot_corr =
        CommsLib::CorrelateAvx(samples_vec, ue_pilot_vec_[ant_id]);
    std::vector<float> pilot_corr_abs = CommsLib::Abs2Avx(pilot_corr);
    size_t peak_offset =
        std::max_element(pilot_corr_abs.begin(), pilot_corr_abs.end()) -
        pilot_corr_abs.begin();
    size_t pilot_offset = peak_offset < seq_len ? 0 : peak_offset - seq_len;
    float noise_power = 0;
    for (size_t i = 0; i < pilot_offset; i++) {
      noise_power += std::pow(std::abs(samples_vec[i]), 2);
    }
    float signal_power = 0;
    for (size_t i = pilot_offset; i < 2 * pilot_offset; i++) {
      signal_power += std::pow(std::abs(samples_vec[i]), 2);
    }
    float snr = 10 * std::log10(signal_power / noise_power);
    std::printf(
        "UeWorker: Fft Pilot(frame %zu symbol %zu ant %zu) sig offset "
        "%zu, SNR %2.1f \n",
        frame_id, symbol_id, ant_id, pilot_offset, snr);
    if (frame_id == kRecordFrameIndex) {
      std::string fname = "rxpilot" + std::to_string(symbol_id) + "_" +
                          std::to_string(ant_id) + ".bin";
      FILE* f = std::fopen(fname.c_str(), "wb");
      std::fwrite(pkt->data_, 2 * sizeof(int16_t), config_.SampsPerSymbol(), f);
      std::fclose(f);
    }
  }

  // remove CP, do FFT
  size_t dl_symbol_id = config_.Frame().GetDLSymbolIdx(symbol_id);
  size_t total_dl_symbol_id =
      (frame_slot * config_.Frame().NumDLSyms()) + dl_symbol_id;
  size_t fft_buffer_target_id =
      (total_dl_symbol_id * config_.UeAntNum()) + ant_id;

  // transfer ushort to float
  size_t delay_offset = (sig_offset + config_.CpLen()) * 2;
  auto* fft_buff = reinterpret_cast<float*>(fft_buffer_[fft_buffer_target_id]);

  SimdConvertShortToFloat(&pkt->data_[delay_offset], fft_buff,
                          config_.OfdmCaNum() * 2);

  // perform fft
  DftiComputeForward(mkl_handle_, fft_buffer_[fft_buffer_target_id]);

  size_t csi_offset = frame_slot * config_.UeAntNum() + ant_id;
  auto* csi_buffer_ptr =
      reinterpret_cast<arma::cx_float*>(csi_buffer_.at(csi_offset).data());
  auto* fft_buffer_ptr =
      reinterpret_cast<arma::cx_float*>(fft_buffer_[fft_buffer_target_id]);

  // In TDD massive MIMO, a pilot symbol needs to be sent
  // in the downlink for the user to estimate the channel
  // due to relative reciprocity calibration,
  // see Argos paper (Mobicom'12)
  if (dl_symbol_id < config_.Frame().ClientDlPilotSymbols()) {
    for (size_t j = 0; j < config_.OfdmDataNum(); j++) {
      complex_float p = config_.UeSpecificPilot()[ant_id][j];
      size_t sc_id = non_null_sc_ind_[j];
      csi_buffer_ptr[j] += (fft_buffer_ptr[sc_id] / arma::cx_float(p.re, p.im));
    }
  }

  if (kDebugPrintPerTaskDone || kDebugPrintFft) {
    size_t fft_duration_stat = GetTime::Rdtsc() - start_tsc;
    std::printf(
        "UeWorker[%zu]: Fft Pilot(frame %zu, symbol %zu, ant %zu) Duration "
        "%2.4f ms\n",
        tid_, frame_id, symbol_id, ant_id,
        GetTime::CyclesToMs(fft_duration_stat, GetTime::MeasureRdtscFreq()));
  }

  // Free the rx buffer
  fft_req_tag_t(tag).rx_packet_->Free();
  EventData fft_finish_event =
      EventData(EventType::kFFTPilot,
                gen_tag_t::FrmSymAnt(frame_id, symbol_id, ant_id).tag_);
  RtAssert(notify_queue_.enqueue(*ptok_.get(), fft_finish_event),
           "UeWorker: FFT Pilot message enqueue failed");
}

void UeWorker::DoDemul(size_t tag) {
  const size_t frame_id = gen_tag_t(tag).frame_id_;
  const size_t symbol_id = gen_tag_t(tag).symbol_id_;
  const size_t ant_id = gen_tag_t(tag).ant_id_;

  if (kDebugPrintInTask || kDebugPrintDemul) {
    std::printf("UeWorker[%zu]: Demul  (frame %zu, symbol %zu, ant %zu)\n",
                tid_, frame_id, symbol_id, ant_id);
  }
  size_t start_tsc = GetTime::Rdtsc();

  const size_t frame_slot = frame_id % kFrameWnd;
  size_t dl_symbol_id = config_.Frame().GetDLSymbolIdx(symbol_id);
  size_t dl_data_symbol_perframe = config_.Frame().NumDlDataSyms();
  size_t total_dl_symbol_id = frame_slot * dl_data_symbol_perframe +
                              dl_symbol_id -
                              config_.Frame().ClientDlPilotSymbols();
  size_t offset = total_dl_symbol_id * config_.UeAntNum() + ant_id;
  auto* equal_ptr = reinterpret_cast<float*>(&equal_buffer_[offset][0]);

  const size_t base_sc_id = 0;

  int8_t* demod_ptr = demod_buffer_[frame_slot][dl_symbol_id][ant_id] +
                      (config_.ModOrderBits() * base_sc_id);

  switch (config_.ModOrderBits()) {
    case (CommsLib::kQpsk):
      DemodQpskSoftSse(equal_ptr, demod_ptr, config_.OfdmDataNum());
      break;
    case (CommsLib::kQaM16):
      Demod16qamSoftAvx2(equal_ptr, demod_ptr, config_.OfdmDataNum());
      break;
    case (CommsLib::kQaM64):
      Demod64qamSoftAvx2(equal_ptr, demod_ptr, config_.OfdmDataNum());
      break;
    default:
      std::printf("UeWorker[%zu]: Demul - modulation type %s not supported!\n",
                  tid_, config_.Modulation().c_str());
  }

  if ((kDebugPrintPerTaskDone == true) || (kDebugPrintDemul == true)) {
    size_t dem_duration_stat = GetTime::Rdtsc() - start_tsc;
    std::printf(
        "UeWorker[%zu]: Demul  (frame %zu, symbol %zu, ant %zu) Duration "
        "%2.4f ms\n",
        tid_, frame_id, symbol_id, ant_id,
        GetTime::CyclesToMs(dem_duration_stat, GetTime::MeasureRdtscFreq()));
  }
  if (kPrintLLRData) {
    std::printf("LLR data, symbol_offset: %zu\n", offset);
    for (size_t i = 0; i < config_.OfdmDataNum(); i++) {
      std::printf("%x ", (uint8_t) * (demod_ptr + i));
    }
    std::printf("\n");
  }

  RtAssert(
      notify_queue_.enqueue(*ptok_.get(), EventData(EventType::kDemul, tag)),
      "Demodulation message enqueue failed");
}

void UeWorker::DoDecodeUe(DoDecodeClient* decoder, size_t tag) {
  const size_t frame_id = gen_tag_t(tag).frame_id_;
  const size_t symbol_id = gen_tag_t(tag).symbol_id_;
  const size_t ant_id = gen_tag_t(tag).ant_id_;

  for (size_t cb_id = 0; cb_id < config_.LdpcConfig().NumBlocksInSymbol();
       cb_id++) {
    // For now, call for each cb
    if (kDebugPrintDecode) {
      std::printf(
          "Decoding [Frame %zu, Symbol %zu, User %zu, Code Block %zu : %zu]\n",
          frame_id, symbol_id, ant_id, cb_id,
          config_.LdpcConfig().NumBlocksInSymbol() - 1);
    }
    decoder->Launch(
        gen_tag_t::FrmSymCb(
            frame_id, symbol_id,
            cb_id + (ant_id * config_.LdpcConfig().NumBlocksInSymbol()))
            .tag_);
  }

  // Post the completion event (symbol)
  size_t completion_tag = gen_tag_t::FrmSymUe(frame_id, symbol_id, ant_id).tag_;

  RtAssert(notify_queue_.enqueue(*ptok_.get(),
                                 EventData(EventType::kDecode, completion_tag)),
           "Decode Symbol message enqueue failed");
}

//////////////////////////////////////////////////////////
//                   UPLINK Operations                //
//////////////////////////////////////////////////////////
void UeWorker::DoEncodeUe(DoEncode* encoder, size_t tag) {
  const size_t frame_id = gen_tag_t(tag).frame_id_;
  const size_t symbol_id = gen_tag_t(tag).symbol_id_;
  const size_t ue_id = gen_tag_t(tag).ue_id_;
  for (size_t ch = 0; ch < config_.NumChannels(); ch++) {
    const size_t ant_id = (ue_id * config_.NumChannels()) + ch;
    // For now, call for each cb
    for (size_t cb_id = 0; cb_id < config_.LdpcConfig().NumBlocksInSymbol();
         cb_id++) {
      // For now, call for each cb
      encoder->Launch(
          gen_tag_t::FrmSymCb(
              frame_id, symbol_id,
              cb_id + (ant_id * config_.LdpcConfig().NumBlocksInSymbol()))
              .tag_);
    }
  }
  // Post the completion event (symbol)
  size_t completion_tag = gen_tag_t::FrmSymUe(frame_id, symbol_id, ue_id).tag_;
  RtAssert(notify_queue_.enqueue(*ptok_.get(),
                                 EventData(EventType::kEncode, completion_tag)),
           "Encoded Symbol message enqueue failed");
}

// This functions accepts non pilot - UL symbols
void UeWorker::DoModul(size_t tag) {
  const size_t frame_id = gen_tag_t(tag).frame_id_;
  const size_t symbol_id = gen_tag_t(tag).symbol_id_;
  const size_t ue_id = gen_tag_t(tag).ue_id_;

  if (kDebugPrintInTask || kDebugPrintModul) {
    std::printf("UeWorker[%zu]: Modul  (frame %zu, symbol %zu, user %zu)\n",
                tid_, frame_id, symbol_id, ue_id);
  }
  size_t start_tsc = GetTime::Rdtsc();

  for (size_t ch = 0; ch < config_.NumChannels(); ch++) {
    const size_t ant_id = (ue_id * config_.NumChannels()) + ch;

    const size_t ul_symbol_idx = config_.Frame().GetULSymbolIdx(symbol_id);
    const size_t total_ul_data_symbol_id =
        config_.GetTotalDataSymbolIdxUl(frame_id, ul_symbol_idx);

    complex_float* modul_buf =
        &modul_buffer_[total_ul_data_symbol_id][ant_id * config_.OfdmDataNum()];

    auto* ul_bits = config_.GetEncodedBuf(encoded_buffer_, frame_id,
                                          ul_symbol_idx, ant_id, 0);

    if (kDebugPrintModul) {
      std::printf(
          "UeWorker[%zu]: Modul  (frame %zu, symbol %zu, user %zu) - getting "
          "from location (%zu %zu %zu) %zu and putting into location (%zu, "
          "%zu) %zu\n\n",
          tid_, frame_id, symbol_id, ue_id, frame_id,
          ul_symbol_idx - config_.Frame().ClientUlPilotSymbols(), ant_id,
          (size_t)ul_bits, total_ul_data_symbol_id,
          ant_id * config_.OfdmDataNum(), (size_t)modul_buf);
    }

    // TODO place directly into the correct location of the fft buffer
    for (size_t sc = 0; sc < config_.OfdmDataNum(); sc++) {
      modul_buf[sc] =
          ModSingleUint8(static_cast<uint8_t>(ul_bits[sc]), config_.ModTable());
    }
  }

  if ((kDebugPrintPerTaskDone == true) || (kDebugPrintModul == true)) {
    size_t mod_duration_stat = GetTime::Rdtsc() - start_tsc;
    std::printf(
        "UeWorker[%zu]: Modul  (frame %zu, symbol %zu, user %zu) Duration "
        "%2.4f ms\n",
        tid_, frame_id, symbol_id, ue_id,
        GetTime::CyclesToMs(mod_duration_stat, GetTime::MeasureRdtscFreq()));
  }

  RtAssert(
      notify_queue_.enqueue(*ptok_.get(), EventData(EventType::kModul, tag)),
      "Modulation complete message enqueue failed");
}

void UeWorker::DoIfftUe(DoIFFTClient* iffter, size_t tag) {
  const size_t frame_id = gen_tag_t(tag).frame_id_;
  const size_t symbol_id = gen_tag_t(tag).symbol_id_;
  const size_t user_id = gen_tag_t(tag).ue_id_;

  // For now, call for each channel
  for (size_t ch = 0; ch < config_.NumChannels(); ch++) {
    size_t ant_id = (user_id * config_.NumChannels()) + ch;

    // TODO Remove this copy
    {
      complex_float const* source_data = nullptr;
      const size_t ul_symbol_idx = config_.Frame().GetULSymbolIdx(symbol_id);
      size_t total_ul_symbol_id =
          config_.GetTotalDataSymbolIdxUl(frame_id, ul_symbol_idx);
      if (ul_symbol_idx < config_.Frame().ClientUlPilotSymbols()) {
        source_data = config_.UeSpecificPilot()[ant_id];
      } else {
        source_data =
            &modul_buffer_[total_ul_symbol_id][ant_id * config_.OfdmDataNum()];
      }
      size_t buff_offset = (total_ul_symbol_id * config_.UeAntNum()) + ant_id;
      complex_float* dest_loc =
          ifft_buffer_[buff_offset] + (config_.OfdmDataStart());
      std::memcpy(dest_loc, source_data,
                  sizeof(complex_float) * config_.OfdmDataNum());
    }

    iffter->Launch(gen_tag_t::FrmSymAnt(frame_id, symbol_id, ant_id).tag_);
  }

  // Post the completion event (symbol)
  size_t completion_tag =
      gen_tag_t::FrmSymUe(frame_id, symbol_id, user_id).tag_;
  RtAssert(notify_queue_.enqueue(*ptok_.get(),
                                 EventData(EventType::kIFFT, completion_tag)),
           "IFFT symbol complete message enqueue failed");
}

void UeWorker::DoIfft(size_t tag) {
  const size_t frame_id = gen_tag_t(tag).frame_id_;
  const size_t symbol_id = gen_tag_t(tag).symbol_id_;
  const size_t user_id = gen_tag_t(tag).ue_id_;

  const size_t frame_slot = frame_id % kFrameWnd;

  if (kDebugPrintInTask) {
    std::printf("User Task[%zu]: iFFT   (frame %zu, symbol %zu, user %zu)\n",
                tid_, frame_id, symbol_id, user_id);
  }
  size_t start_tsc = GetTime::Rdtsc();

  for (size_t ch = 0; ch < config_.NumChannels(); ch++) {
    const size_t ul_symbol_perframe = config_.Frame().NumULSyms();

    size_t ul_symbol_id = config_.Frame().GetULSymbolIdx(symbol_id);
    size_t ant_id = user_id * config_.NumChannels() + ch;
    size_t total_ul_symbol_id = frame_slot * ul_symbol_perframe + ul_symbol_id;
    size_t buff_offset = total_ul_symbol_id * config_.UeAntNum() + ant_id;
    complex_float* ifft_buff = ifft_buffer_[buff_offset];

    std::memset(ifft_buff, 0, sizeof(complex_float) * config_.OfdmDataStart());
    if (ul_symbol_id < config_.Frame().ClientUlPilotSymbols()) {
      std::memcpy(ifft_buff + config_.OfdmDataStart(),
                  config_.UeSpecificPilot()[ant_id],
                  config_.OfdmDataNum() * sizeof(complex_float));
    } else {
      complex_float* modul_buff =
          &modul_buffer_[total_ul_symbol_id][ant_id * config_.OfdmDataNum()];
      std::memcpy(ifft_buff + config_.OfdmDataStart(), modul_buff,
                  config_.OfdmDataNum() * sizeof(complex_float));
    }
    std::memset(ifft_buff + config_.OfdmDataStop(), 0,
                sizeof(complex_float) * config_.OfdmDataStart());

    CommsLib::IFFT(ifft_buff, config_.OfdmCaNum(), false);

    size_t tx_offset = buff_offset * config_.PacketLength();
    char* cur_tx_buffer = &tx_buffer_[tx_offset];
    auto* pkt = reinterpret_cast<struct Packet*>(cur_tx_buffer);
    auto* tx_data_ptr = reinterpret_cast<std::complex<short>*>(pkt->data_);
    CommsLib::Ifft2tx(ifft_buff, tx_data_ptr, config_.OfdmCaNum(),
                      config_.OfdmTxZeroPrefix(), config_.CpLen(),
                      config_.Scale());
  }

  if ((kDebugPrintPerTaskDone == true)) {
    size_t ifft_duration_stat = GetTime::Rdtsc() - start_tsc;
    std::printf(
        "User Task[%zu]: iFFT   (frame %zu,       , user %zu) Duration "
        "%2.4f ms\n",
        tid_, frame_id, user_id,
        GetTime::CyclesToMs(ifft_duration_stat, GetTime::MeasureRdtscFreq()));
  }

  // Post the completion event (symbol)
  size_t completion_tag =
      gen_tag_t::FrmSymUe(frame_id, symbol_id, user_id).tag_;
  RtAssert(notify_queue_.enqueue(*ptok_.get(),
                                 EventData(EventType::kIFFT, completion_tag)),
           "IFFT symbol complete message enqueue failed");
}

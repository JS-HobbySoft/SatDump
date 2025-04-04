#include "module_aero_decoder.h"
#include <fstream>
#include "logger.h"
#include <filesystem>
#include "common/widgets/themed_widgets.h"
#include "common/utils.h"
#include "common/codings/rotation.h"
#include <iostream>
#include <iomanip>
#include <sstream>

namespace inmarsat
{
    namespace aero
    {
        AeroDecoderModule::AeroDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters) : ProcessingModule(input_file, output_file_hint, parameters)
        {
            is_c_channel = parameters.contains("is_c") ? parameters["is_c"].get<bool>() : false;

            d_aero_oqpsk = parameters["oqpsk"].get<bool>();
            d_aero_dummy_bits = parameters["dummy_bits"].get<int>();
            d_aero_interleaver_cols = parameters["inter_cols"].get<int>();
            d_aero_interleaver_blocks = parameters["inter_blocks"].get<int>();

            d_aero_ber_thresold = parameters.contains("ber_thresold") ? parameters["ber_thresold"].get<float>() : 1.0;

            if (parameters.contains("vfo_freq"))
            {
                freq_for_info_log = parameters["vfo_freq"].get<double>();
            }
            else
            {
                freq_for_info_log = 0;
            }

            if (parameters.contains("vfo_name"))
            {
                name_for_info_log = parameters["vfo_name"].get<std::string>();
            }
            else
            {
                name_for_info_log = "none";
            }

            if (is_c_channel)
                d_aero_sync_size = 52 * 2;
            else
                d_aero_sync_size = d_aero_oqpsk ? 64 : 32;
            if (is_c_channel)
                d_aero_hdr_size = d_aero_dummy_bits;
            else
                d_aero_hdr_size = 16 + d_aero_dummy_bits;
            d_aero_interleaver_block_size = 64 * d_aero_interleaver_cols;
            d_aero_info_size = d_aero_interleaver_block_size * d_aero_interleaver_blocks;
            d_aero_total_frm_size = d_aero_sync_size + d_aero_hdr_size + d_aero_info_size;

            logger->info("Aero Sync Size : %d", d_aero_sync_size);
            logger->info("Aero Header Size : %d", d_aero_hdr_size);
            logger->info("Aero Info Size : %d", d_aero_info_size);
            logger->info("Aero Frame Size : %d", d_aero_total_frm_size);

            if (is_c_channel)
            {
                std::vector<uint8_t> bits = {
                    1, 0, 0, 0, 1, 0, 0, 0, //
                    1, 1, 0, 1, 1, 0, 1, 0, //
                    0, 0, 0, 1, 1, 0, 1, 1, //
                    0, 0, 1, 0, 1, 1, 1, 1, //
                    0, 1, 1, 1, 1, 0, 0, 1, //
                    1, 0, 0, 0, 0, 0, 1, 1, //
                    0, 1, 0, 1, 1, 0, 1, 0, //
                    1, 1, 0, 0, 0, 0, 0, 1, //
                    1, 0, 0, 1, 1, 1, 1, 0, //
                    1, 1, 1, 1, 0, 1, 0, 0, //
                    1, 1, 0, 1, 1, 0, 0, 0, //
                    0, 1, 0, 1, 1, 0, 1, 1, //
                    0, 0, 0, 1, 0, 0, 0, 1, //
                };
                correlator = std::make_unique<CorrelatorGeneric>(d_aero_oqpsk ? dsp::OQPSK : dsp::BPSK,
                                                                 bits,
                                                                 d_aero_total_frm_size);
                d_aero_info_size = 5460;
            }
            else
            {
                correlator = std::make_unique<CorrelatorGeneric>(d_aero_oqpsk ? dsp::OQPSK : dsp::BPSK,
                                                                 d_aero_oqpsk ? unsigned_to_bitvec<uint64_t>(0b1111110000000011001100111100110011111100110000001100001100001111)
                                                                              : unsigned_to_bitvec<uint32_t>(0b11100001010110101110100010010011),
                                                                 d_aero_total_frm_size);
            }

            viterbi = std::make_unique<viterbi::Viterbi27>(d_aero_info_size / 2, std::vector<int>{109, 79}, d_aero_info_size / 5);

            // Generate randomization sequence
            {
                uint16_t shifter = 0b100110101001011; /// 0b110100101011001;
                int cpos = 0;
                uint8_t shifter2 = 0;
                for (int i = 0; i < d_aero_info_size; i++)
                {
                    uint8_t x1 = (shifter >> 0) & 1;
                    uint8_t x15 = (shifter >> 14) & 1;
                    uint8_t newb = x1 ^ x15;
                    shifter = shifter << 1 | newb;
                    shifter2 = shifter2 << 1 | newb;
                    cpos++;
                    if (cpos == 8)
                    {
                        cpos = 0;
                        randomization_seq.push_back(shifter2);
                    }
                }
            }

            soft_buffer = new int8_t[d_aero_total_frm_size];
            buffer_deinterleaved = new int8_t[d_aero_info_size];
            buffer_vitdecoded = new uint8_t[d_aero_info_size];
        }

        std::vector<ModuleDataType> AeroDecoderModule::getInputTypes()
        {
            return {DATA_FILE, DATA_STREAM};
        }

        std::vector<ModuleDataType> AeroDecoderModule::getOutputTypes()
        {
            return {DATA_FILE, DATA_STREAM};
        }

        AeroDecoderModule::~AeroDecoderModule()
        {
            delete[] soft_buffer;
            delete[] buffer_deinterleaved;
            delete[] buffer_vitdecoded;
        }

        uint8_t reverseBits(uint8_t byte)
        {
            byte = (byte & 0xF0) >> 4 | (byte & 0x0F) << 4;
            byte = (byte & 0xCC) >> 2 | (byte & 0x33) << 2;
            byte = (byte & 0xAA) >> 1 | (byte & 0x55) << 1;
            return byte;
        }

        void AeroDecoderModule::process()
        {
            if (input_data_type == DATA_FILE)
                filesize = getFilesize(d_input_file);
            else
                filesize = 0;
            if (input_data_type == DATA_FILE)
                data_in = std::ifstream(d_input_file, std::ios::binary);
            if (output_data_type == DATA_FILE)
            {
                data_out = std::ofstream(d_output_file_hint + ".frm", std::ios::binary);
                d_output_files.push_back(d_output_file_hint + ".frm");
            }

            logger->info("Using input symbols " + d_input_file);
            logger->info("Decoding to " + d_output_file_hint + ".frm");

            phase_t phase;
            bool swap;

            uint8_t *depunc_out = nullptr;

            if (is_c_channel)
                depunc_out = new uint8_t[d_aero_info_size];

            time_t lastTime = 0;
            while (input_data_type == DATA_FILE ? !data_in.eof() : input_active.load())
            {
                // Read a buffer
                if (input_data_type == DATA_FILE)
                    data_in.read((char *)soft_buffer, d_aero_total_frm_size);
                else
                    input_fifo->read((uint8_t *)soft_buffer, d_aero_total_frm_size);

                int pos = correlator->correlate((int8_t *)soft_buffer, phase, swap, correlator_cor, d_aero_total_frm_size);

                correlator_locked = pos == 0; // Update locking state

                if (pos != 0 && pos < d_aero_total_frm_size) // Safety
                {
                    memmove(soft_buffer, &soft_buffer[pos], d_aero_total_frm_size - pos);

                    if (input_data_type == DATA_FILE)
                        data_in.read((char *)&soft_buffer[d_aero_total_frm_size - pos], pos);
                    else
                        input_fifo->read((uint8_t *)&soft_buffer[d_aero_total_frm_size - pos], pos);
                }

                // Correct phase ambiguity
                if (d_aero_oqpsk)
                {
                    rotate_soft((int8_t *)soft_buffer, d_aero_total_frm_size, phase, false);

                    if (swap)
                    {
                        int8_t last_q_oqpsk = 0;
                        for (int i = (d_aero_total_frm_size / 2) - 1; i >= 0; i--)
                        {
                            int8_t back = soft_buffer[i * 2 + 1];
                            soft_buffer[i * 2 + 1] = last_q_oqpsk;
                            last_q_oqpsk = back;
                        }
                    }
                }

                // Deinterleave
                for (int i = 0; i < d_aero_interleaver_blocks; i++)
                    deinterleave(&soft_buffer[d_aero_sync_size + d_aero_hdr_size + d_aero_interleaver_block_size * i],
                                 &buffer_deinterleaved[d_aero_interleaver_block_size * i],
                                 d_aero_interleaver_cols);

                if (is_c_channel) // Call packets
                {
                    depuncture(buffer_deinterleaved, depunc_out, 2, d_aero_interleaver_block_size * d_aero_interleaver_blocks - 1);

                    viterbi->work((int8_t *)depunc_out, buffer_vitdecoded, true);

                    if (viterbi->ber() < d_aero_ber_thresold)
                    {
                        // Derand
                        for (int i = 0; i < d_aero_info_size / 16; i++)
                            buffer_vitdecoded[i] ^= randomization_seq[i];

                        std::vector<uint8_t> voice_data(300), blocks_data(36);
                        unpack_areo_c84_packet(buffer_vitdecoded, voice_data.data(), blocks_data.data());

                        memcpy(buffer_vitdecoded, blocks_data.data(), blocks_data.size());
                        memcpy(&buffer_vitdecoded[36], voice_data.data(), voice_data.size());

                        if (output_data_type == DATA_FILE)
                            data_out.write((char *)buffer_vitdecoded, 336);
                        else
                            output_fifo->write((uint8_t *)buffer_vitdecoded, 336);
                    }
                }
                else // Normal
                {
                    viterbi->work(buffer_deinterleaved, buffer_vitdecoded);

                    if (viterbi->ber() < d_aero_ber_thresold)
                    {
                        // Derand
                        for (int i = 0; i < d_aero_info_size / 16; i++)
                        {
                            buffer_vitdecoded[i] ^= randomization_seq[i];
                            buffer_vitdecoded[i] = reverseBits(buffer_vitdecoded[i]);
                        }

                        if (output_data_type == DATA_FILE)
                            data_out.write((char *)buffer_vitdecoded, d_aero_info_size / 16);
                        else
                            output_fifo->write((uint8_t *)buffer_vitdecoded, d_aero_info_size / 16);
                    }
                }

                if (input_data_type == DATA_FILE)
                    progress = data_in.tellg();

                // Update module stats
                module_stats["correlator_lock"] = correlator_locked;
                module_stats["correlator_corr"] = correlator_cor;
                module_stats["viterbi_ber"] = viterbi->ber();

                std::stringstream ss;
                ss << std::fixed << std::setprecision(0) << freq_for_info_log;
                std::string freq_for_log = ss.str();

                if (time(NULL) % 10 == 0 && lastTime != time(NULL))
                {
                    lastTime = time(NULL);
                    std::string lock_state = correlator_locked ? "SYNCED" : "NOSYNC";
                    logger->info("VFO: " + name_for_info_log + " Freq: " + freq_for_log + ", Viterbi BER : " + std::to_string(viterbi->ber() * 100) + "%%, Lock : " + lock_state);
                }
            }

            if (is_c_channel)
                delete[] depunc_out;

            if (input_data_type == DATA_FILE)
                data_in.close();
            if (output_data_type == DATA_FILE)
                data_out.close();
        }

        void AeroDecoderModule::drawUI(bool window)
        {
            ImGui::Begin("Inmarsat Aero Decoder", NULL, window ? 0 : NOWINDOW_FLAGS);

            float ber = viterbi->ber();

            ImGui::BeginGroup();
            {
                ImGui::Button("Correlator", {200 * ui_scale, 20 * ui_scale});
                {
                    ImGui::Text("Corr  : ");
                    ImGui::SameLine();
                    ImGui::TextColored(correlator_locked ? style::theme.green : style::theme.orange, UITO_C_STR(correlator_cor));

                    std::memmove(&cor_history[0], &cor_history[1], (200 - 1) * sizeof(float));
                    cor_history[200 - 1] = correlator_cor;

                    widgets::ThemedPlotLines(style::theme.plot_bg.Value, "", cor_history, IM_ARRAYSIZE(cor_history), 0, "", 25.0f, 64.0f,
                        ImVec2(200 * ui_scale, 50 * ui_scale));
                }

                ImGui::Button("Viterbi", {200 * ui_scale, 20 * ui_scale});
                {
                    ImGui::Text("BER   : ");
                    ImGui::SameLine();
                    ImGui::TextColored(ber < 0.22 ? style::theme.green : style::theme.red, UITO_C_STR(ber));

                    std::memmove(&ber_history[0], &ber_history[1], (200 - 1) * sizeof(float));
                    ber_history[200 - 1] = ber;

                    widgets::ThemedPlotLines(style::theme.plot_bg.Value, "", ber_history, IM_ARRAYSIZE(ber_history), 0, "", 0.0f, 1.0f,
                        ImVec2(200 * ui_scale, 50 * ui_scale));
                }
            }
            ImGui::EndGroup();

            if (input_data_type == DATA_FILE)
                ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

            ImGui::End();
        }

        std::string AeroDecoderModule::getID()
        {
            return "inmarsat_aero_decoder";
        }

        std::vector<std::string> AeroDecoderModule::getParameters()
        {
            return {};
        }

        std::shared_ptr<ProcessingModule> AeroDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
        {
            return std::make_shared<AeroDecoderModule>(input_file, output_file_hint, parameters);
        }
    }
}

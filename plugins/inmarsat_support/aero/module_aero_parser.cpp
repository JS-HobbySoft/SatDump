#include "module_aero_parser.h"
#include <fstream>
#include "logger.h"
#include <filesystem>
#include "imgui/imgui.h"
#include "core/config.h"
#include "common/utils.h"
#include "decode_utils.h"
#include "common/dsp/io/wav_writer.h"
#include "common/audio/audio_sink.h"
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

#define SIGNAL_UNIT_SIZE_BYTES 12

namespace inmarsat
{
    namespace aero
    {
        std::vector<std::string> stringList = 
        {
            "SSU", 
            "AES System Table Broadcast (Index)", 
            "Reserved 0x26",
            "Acknowledge (RACK / TACK P Channel, PACK R Channel)",
            "T Channel Assignment"
        };
        AeroParserModule::AeroParserModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters) : ProcessingModule(input_file, output_file_hint, parameters)
        {
            buffer = new uint8_t[SIGNAL_UNIT_SIZE_BYTES];
            memset(buffer, 0, SIGNAL_UNIT_SIZE_BYTES);

            is_c_channel = parameters.contains("is_c") ? parameters["is_c"].get<bool>() : false;

            if (parameters.contains("udp_sinks"))
            {
                for (auto &sinks : parameters["udp_sinks"].items())
                {
                    std::string address = sinks.value()["address"].get<std::string>();
                    int port = sinks.value()["port"].get<int>();
                    udp_clients.push_back(std::make_shared<net::UDPClient>((char *)address.c_str(), port));
                }
            }

            if (parameters.contains("save_files"))
                do_save_files = parameters["save_files"].get<bool>();
            else
                do_save_files = true;

            if (parameters.contains("station_id"))
                d_station_id = parameters["station_id"].get<std::string>();

            play_audio = satdump::config::main_cfg["user_interface"]["play_audio"]["value"].get<bool>();
        }

        std::vector<ModuleDataType> AeroParserModule::getInputTypes()
        {
            return {DATA_FILE, DATA_STREAM};
        }

        std::vector<ModuleDataType> AeroParserModule::getOutputTypes()
        {
            return {DATA_FILE};
        }

        AeroParserModule::~AeroParserModule()
        {
            delete[] buffer;
        }

        std::string timestampToTod(time_t time_v)
        {
            std::tm *timeReadable = gmtime(&time_v);
            return (timeReadable->tm_hour > 9 ? std::to_string(timeReadable->tm_hour) : "0" + std::to_string(timeReadable->tm_hour)) + ":" + // Hour HH
                   (timeReadable->tm_min > 9 ? std::to_string(timeReadable->tm_min) : "0" + std::to_string(timeReadable->tm_min)) + ":" +    // Minutes mm
                   (timeReadable->tm_sec > 9 ? std::to_string(timeReadable->tm_sec) : "0" + std::to_string(timeReadable->tm_sec));           // Seconds ss
        }

        void AeroParserModule::process_final_pkt(nlohmann::json &msg)
        {
            std::string pkt_name = "";
            if (msg.contains("msg_name"))
                pkt_name = msg["msg_name"];
            else
                return;
            for (auto &c : pkt_name)
                if (c == '/')
                    c = '_';

            // UDP
            {
                for (auto &c : udp_clients)
                {
                    try
                    {
                        nlohmann::json msg2 = msg;
                        if (d_station_id != ""){
                            msg2["source"]["station_id"] = d_station_id;
                            msg2["source"]["app"]["name"] = "SatDump";
                            msg2["source"]["app"]["version"] = (std::string)SATDUMP_VERSION;
                        }
                        
                        std::string m = msg2.dump();
                        c->send((uint8_t *)m.data(), m.size());
                    }
                    catch (std::exception &e)
                    {
                        logger->error("Error sending to UDP! %s", e.what());
                    }
                }
            }

            // File
            if (do_save_files)
            {
                std::string directory = d_output_file_hint.substr(0, d_output_file_hint.rfind('/')) + "/" + pkt_name;

                if (!std::filesystem::exists(directory))
                    std::filesystem::create_directory(directory);

                double time_d = msg["timestamp"].get<double>();
                time_t time_v = time_d;
                std::tm *timeReadable = gmtime(&time_v);

                std::string utc_filename = std::to_string(timeReadable->tm_year + 1900) +                                                                               // Year yyyy
                                           (timeReadable->tm_mon + 1 > 9 ? std::to_string(timeReadable->tm_mon + 1) : "0" + std::to_string(timeReadable->tm_mon + 1)) + // Month MM
                                           (timeReadable->tm_mday > 9 ? std::to_string(timeReadable->tm_mday) : "0" + std::to_string(timeReadable->tm_mday)) + "T" +    // Day dd
                                           (timeReadable->tm_hour > 9 ? std::to_string(timeReadable->tm_hour) : "0" + std::to_string(timeReadable->tm_hour)) +          // Hour HH
                                           (timeReadable->tm_min > 9 ? std::to_string(timeReadable->tm_min) : "0" + std::to_string(timeReadable->tm_min)) +             // Minutes mm
                                           (timeReadable->tm_sec > 9 ? std::to_string(timeReadable->tm_sec) : "0" + std::to_string(timeReadable->tm_sec)) + "Z";        // Seconds ss

                if (std::find(stringList.begin(), stringList.end(), pkt_name) == stringList.end()) {
                    std::string path = directory + "/" + utc_filename + ".json";
                    int i = 1;
                    while (std::filesystem::exists(path))
                        path = directory + "/" + utc_filename + "_" + std::to_string(i++) + ".json";

                    std::ofstream outf(path, std::ios::binary);
                    outf << msg.dump(4);
                    outf.close();
                }
            }
        }

        void AeroParserModule::process_pkt()
        {
            if (check_crc(buffer))
            {
                uint8_t pkt_id = buffer[0];
                nlohmann::json final_pkt;

                try
                {
                    switch (pkt_id)
                    {
                    case pkts::MessageAESSystemTableBroadcastIndex::MSG_ID:
                        final_pkt = pkts::MessageAESSystemTableBroadcastIndex(buffer);
                        break;

                    case pkts::MessageUserDataISU::MSG_ID:
                        wip_user_data.isu = pkts::MessageUserDataISU(buffer);
                        wip_user_data.ssu.clear();
                        has_wip_user_data = true;
                        break;

                    // Do NOTHING on Reserved
                    case 0x26:
                        break;

                    // SSUs
                    default:
                        if ((pkt_id & 0xC0) == 0xC0)
                        {
                            if (has_wip_user_data)
                            {
                                auto ssu = pkts::MessageUserDataSSU(buffer);
                                wip_user_data.ssu.push_back(ssu);

                                if (ssu.seq_no == 0) // Process
                                {
                                    auto payload = wip_user_data.get_payload();

                                    if (acars::is_acars_data(payload))
                                    {
                                        auto ac = acars_parser.parse(payload);
                                        if (ac.has_value())
                                        {
                                            final_pkt = ac.value();
                                            final_pkt["msg_name"] = "ACARS";
                                            final_pkt["signal_unit"] = wip_user_data.isu;
                                            auto libac = acars::parse_libacars(ac.value(), la_msg_dir::LA_MSG_DIR_GND2AIR);
                                            if (!libac.empty())
                                                final_pkt["libacars"] = libac;
                                            // logger->critical(final_pkt["libacars"].dump(4));
                                            logger->info("ACARS message (%s) : \n%s",
                                                         final_pkt["plane_reg"].get<std::string>().c_str(),
                                                         final_pkt["message"].get<std::string>().c_str());
                                        }
                                    }

                                    has_wip_user_data = false;
                                }
                            }
                        }
                        else
                        {
                            std::string name = pkt_type_to_name(pkt_id);
                            if (std::find(stringList.begin(), stringList.end(), name) == stringList.end()) {
                                logger->debug(name);
                            }
                        }
                        break;
                    }

                    if (!final_pkt.contains("msg_name"))
                    {
                        std::string name = pkt_type_to_name(pkt_id);
                        if (std::find(stringList.begin(), stringList.end(), name) == stringList.end()) {
                            if (name.find("Reserved") == std::string::npos)
                                final_pkt["msg_name"] = name;
                            logger->info("Packet : " + name);
                        }
                    }

                    final_pkt["timestamp"] = time(0);

                    if (is_gui)
                    {
                        if (final_pkt.contains("msg_name"))
                        {
                            pkt_history_mtx.lock();
                            if (final_pkt["msg_name"].get<std::string>() == "ACARS")
                            {
                                pkt_history_acars.push_back(final_pkt);
                                if (pkt_history_acars.size() > 200)
                                    pkt_history_acars.erase(pkt_history_acars.begin());
                            }
                            else
                            {
                                pkt_history.push_back(final_pkt);
                                if (pkt_history.size() > 200)
                                    pkt_history.erase(pkt_history.begin());
                            }
                            pkt_history_mtx.unlock();
                        }
                    }

                    process_final_pkt(final_pkt);
                }
                catch (std::exception &e)
                {
                    logger->error("Error processing Aero frames : %s", e.what());
                }
            }
            else
            {
                // logger->error("Invalid CRC!");
            }
        }

        void AeroParserModule::process()
        {
            if (input_data_type == DATA_FILE)
                filesize = getFilesize(d_input_file);
            else
                filesize = 0;
            if (input_data_type == DATA_FILE)
                data_in = std::ifstream(d_input_file, std::ios::binary);

            logger->info("Using input frames " + d_input_file);

            uint8_t *voice_data = nullptr;
            AmbeDecoder *ambed = nullptr;
            int16_t *audio_out = nullptr;
            std::ofstream *file_wav = nullptr;
            dsp::WavWriter *wav_out = nullptr;
            uint64_t final_wav_size = 0;
            if (is_c_channel)
            {
                voice_data = new uint8_t[300];
                ambed = new AmbeDecoder();
                audio_out = new int16_t[160 * 25];
                std::string directory = d_output_file_hint.substr(0, d_output_file_hint.rfind('/'));
                file_wav = new std::ofstream(directory + "/audio.wav", std::ios::binary);
                wav_out = new dsp::WavWriter(*file_wav);
                wav_out->write_header(8000, 1);
            }

            std::shared_ptr<audio::AudioSink> audio_sink;
            if (input_data_type != DATA_FILE && audio::has_sink() && is_c_channel)
            {
                enable_audio = true;
                audio_sink = audio::get_default_sink();
                audio_sink->set_samplerate(8000);
                audio_sink->start();
            }

            time_t lastTime = 0;
            while (input_data_type == DATA_FILE ? !data_in.eof() : input_active.load())
            {
                if (is_c_channel)
                {
                    for (int i = 0; i < 3; i++)
                    {
                        // Read a buffer
                        if (input_data_type == DATA_FILE)
                            data_in.read((char *)buffer, SIGNAL_UNIT_SIZE_BYTES);
                        else
                            input_fifo->read((uint8_t *)buffer, SIGNAL_UNIT_SIZE_BYTES);

                        process_pkt();
                    }

                    // Read a buffer
                    if (input_data_type == DATA_FILE)
                        data_in.read((char *)voice_data, 300);
                    else
                        input_fifo->read((uint8_t *)voice_data, 300);

                    ambed->decode(voice_data, 25, audio_out);

                    file_wav->write((char *)audio_out, 320 * 25);
                    final_wav_size += 320 * 25;

                    if (enable_audio && play_audio)
                        audio_sink->push_samples(audio_out, 160 * 25);
                }
                else
                {
                    // Read a buffer
                    if (input_data_type == DATA_FILE)
                        data_in.read((char *)buffer, SIGNAL_UNIT_SIZE_BYTES);
                    else
                        input_fifo->read((uint8_t *)buffer, SIGNAL_UNIT_SIZE_BYTES);

                    process_pkt();
                }

                if (input_data_type == DATA_FILE)
                    progress = data_in.tellg();

                if (time(NULL) % 10 == 0 && lastTime != time(NULL))
                {
                    lastTime = time(NULL);
//                    logger->info("Progress " + std::to_string(round(((double)progress / (double)filesize) * 1000.0) / 10.0) + "%%");
                }
            }

            if (enable_audio)
                audio_sink->stop();

            if (is_c_channel)
            {
                delete[] voice_data;
                delete ambed;
                delete[] audio_out;
                wav_out->finish_header(final_wav_size);
                file_wav->close();
                delete file_wav;
                delete wav_out;
            }

            if (input_data_type == DATA_FILE)
                data_in.close();
        }

        void AeroParserModule::drawUI(bool window)
        {
            is_gui = true;

            ImGui::Begin("Inmarsat Aero Parser", NULL, window ? 0 : NOWINDOW_FLAGS);

            ImGui::Text("Decoded packets can be seen in a floating window.");
            ImGui::Spacing();
            ImGui::TextColored(style::theme.red, "Note : Still WIP!");

            ImGui::Text("Do remember you should not keep nor share data that is\nnot intended for you.");

            if (enable_audio)
            {
                ImGui::Spacing();
                const char *btn_icon, *label;
                ImVec4 color;
                if (play_audio)
                {
                    color = style::theme.green.Value;
                    btn_icon = u8"\uF028##aeroaudio";
                    label = "Audio Playing";
                }
                else
                {
                    color = style::theme.red.Value;
                    btn_icon = u8"\uF026##aeroaudio";
                    label = "Audio Muted";
                }

                ImGui::PushStyleColor(ImGuiCol_Text, color);
                if (ImGui::Button(btn_icon))
                    play_audio = !play_audio;
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextUnformatted(label);
            }

            if (input_data_type == DATA_FILE)
                ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

            ImGui::End();

            if (input_data_type != DATA_FILE)
            {
                ImGui::Begin("Aero Packets", NULL, ImGuiWindowFlags_HorizontalScrollbar);

                pkt_history_mtx.lock();
                ImGui::BeginTabBar("##aeromessagestabbar");
                if (ImGui::BeginTabItem("ACARS"))
                {
                    float wrap_pos = ImGui::GetContentRegionMax().x;
                    ImGui::BeginTable("##aeroacardstable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit);
                    ImGui::TableSetupColumn("Plane", ImGuiTableColumnFlags_NoResize, 150 * ui_scale);
                    ImGui::TableSetupColumn("Timestamp", ImGuiTableColumnFlags_NoResize, 75 * ui_scale);
                    ImGui::TableSetupColumn("Contents", 0, -1);
                    ImGui::TableHeadersRow();

                    for (int i = pkt_history_acars.size() - 1; i >= 0; i--)
                    {
                        auto &msg = pkt_history_acars[i];
                        try
                        {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextColored(style::theme.lavender, "%s", msg["plane_reg"].get<std::string>().c_str());
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextColored(style::theme.yellow, "%s", timestampToTod(msg["timestamp"].get<double>()).c_str());
                            ImGui::TableSetColumnIndex(2);
                            ImGui::PushTextWrapPos(wrap_pos);
                            ImGui::TextColored(style::theme.green, "%s", msg["message"].get<std::string>().c_str());
                            ImGui::PopTextWrapPos();
                        }
                        catch (std::exception &)
                        {
                        }
                    }
                    ImGui::EndTable();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Packets"))
                {
                    float wrap_pos = ImGui::GetContentRegionMax().x;
                    ImGui::BeginTable("##aeromessagetable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit);
                    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_NoResize, 150 * ui_scale);
                    ImGui::TableSetupColumn("Timestamp", ImGuiTableColumnFlags_NoResize, 75 * ui_scale);
                    ImGui::TableSetupColumn("Contents", 0, -1);
                    ImGui::TableHeadersRow();

                    for (int i = pkt_history.size() - 1; i >= 0; i--)
                    {
                        auto &msg = pkt_history[i];
                        try
                        {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextColored(style::theme.lavender, "%s", msg["msg_name"].get<std::string>().c_str());
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextColored(style::theme.yellow, "%s", timestampToTod(msg["timestamp"].get<double>()).c_str());
                            ImGui::TableSetColumnIndex(2);
                            ImGui::PushTextWrapPos(wrap_pos);
                            ImGui::TextColored(style::theme.green, "%s", msg.dump().c_str());
                            ImGui::PopTextWrapPos();
                        }
                        catch (std::exception &)
                        {
                        }
                    }
                    ImGui::EndTable();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
                pkt_history_mtx.unlock();

                ImGui::End();
            }
        }

        std::string AeroParserModule::getID()
        {
            return "inmarsat_aero_parser";
        }

        std::vector<std::string> AeroParserModule::getParameters()
        {
            return {};
        }

        std::shared_ptr<ProcessingModule> AeroParserModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
        {
            return std::make_shared<AeroParserModule>(input_file, output_file_hint, parameters);
        }
    }
}

// Live (non-render) mode for the headless Vital build.
// Header-only: audio output via ALSA, MIDI via a virtual ALSA port plus
// auto-enabled hardware ports, Program Change -> patch file map from JSON.
#pragma once

#include "JuceHeader.h"
#include "load_save.h"
#include "sound_engine.h"
#include "synth_base.h"
#include "synth_constants.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <string>

namespace live {
  inline volatile std::sig_atomic_t& quitFlag() {
    static volatile std::sig_atomic_t flag = 0;
    return flag;
  }
  inline void signalHandler(int) { quitFlag() = 1; }

  // createNewDevice returns a raw pointer in older JUCE and a unique_ptr in newer; accept both.
  inline std::unique_ptr<MidiInput> wrapMidiInput(MidiInput* input) { return std::unique_ptr<MidiInput>(input); }
  inline std::unique_ptr<MidiInput> wrapMidiInput(std::unique_ptr<MidiInput> input) { return input; }

  inline void log(const String& message) { std::cout << message.toRawUTF8() << std::endl; }
}

class LiveSynth : public HeadlessSynth, public AudioSource, private Timer {
  public:
    struct Config {
      String midi_port_name = "Vital";
      String audio_device;                       // empty = system default
      double sample_rate = vital::kDefaultSampleRate;
      int buffer_size = 256;
      int default_program = -1;                  // -1 = init preset
      int program_channel = 0;                   // 0 = any channel, 1-16 = only that channel
      std::map<int, File> programs;              // Program Change number -> .vital file
    };

    static bool loadConfig(const File& config_file, Config& config, String& error) {
      try {
        json data = json::parse(config_file.loadFileAsString().toStdString(), nullptr);
        if (!data.is_object()) {
          error = "Config root must be a JSON object.";
          return false;
        }

        config.midi_port_name = String::fromUTF8(data.value("midi_port_name", std::string("Vital")).c_str());
        config.audio_device = String::fromUTF8(data.value("audio_device", std::string()).c_str());
        config.sample_rate = data.value("sample_rate", (double)vital::kDefaultSampleRate);
        config.buffer_size = data.value("buffer_size", 256);
        config.default_program = data.value("default_program", -1);
        config.program_channel = data.value("program_channel", 0);

        File base = config_file.getParentDirectory();
        if (data.count("patch_dir"))
          base = base.getChildFile(String::fromUTF8(data["patch_dir"].get<std::string>().c_str()));

        if (!data.count("programs") || !data["programs"].is_object()) {
          error = "Config needs a \"programs\" object, e.g. {\"0\": \"Bass.vital\"}.";
          return false;
        }

        for (auto it = data["programs"].begin(); it != data["programs"].end(); ++it) {
          int program = std::stoi(it.key());
          if (program < 0 || program > 127)
            continue;
          config.programs[program] = base.getChildFile(String::fromUTF8(it.value().get<std::string>().c_str()));
        }
      }
      catch (const std::exception& e) {
        error = String("Bad config: ") + e.what();
        return false;
      }
      return true;
    }

    explicit LiveSynth(const Config& config) : config_(config) { router_.owner = this; }
    ~LiveSynth() override { stop(); }

    bool start() {
      loadPatches();

      if (config_.default_program >= 0 && patches_.count(config_.default_program))
        loadProgram(config_.default_program);
      else
        loadInitPreset();

      if (!startAudio())
        return false;

      startMidi();
      startTimer(kPollMs);
      return true;
    }

    void stop() {
      stopTimer();
      device_manager_.removeAudioCallback(&audio_player_);
      audio_player_.setSource(nullptr);
      device_manager_.removeMidiInputCallback(String(), &router_);
      if (virtual_port_) {
        virtual_port_->stop();
        virtual_port_.reset();
      }
      device_manager_.closeAudioDevice();
    }

    // AudioSource
    void prepareToPlay(int buffer_size, double sample_rate) override {
      sample_rate_ = sample_rate;
      engine_->setSampleRate(sample_rate);
      engine_->updateAllModulationSwitches();
      midi_manager_->setSampleRate(sample_rate);
    }

    void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
      AudioSampleBuffer* buffer = info.buffer;
      const int num_samples = info.numSamples;
      const int channels = std::min(buffer->getNumChannels(), vital::kNumChannels);

      ScopedLock lock(getCriticalSection());

      processModulationChanges();
      MidiBuffer midi_messages;
      midi_manager_->removeNextBlockOfMessages(midi_messages, num_samples);

      const int synth_samples = std::min(num_samples, vital::kMaxBufferSize);
      const double sample_time = 1.0 / sample_rate_;
      for (int b = 0; b < num_samples; b += synth_samples) {
        int current_samples = std::min(synth_samples, num_samples - b);
        engine_->correctToTime(current_time_);

        processMidi(midi_messages, b, b + current_samples);
        processAudio(buffer, channels, current_samples, info.startSample + b);
        current_time_ += current_samples * sample_time;
      }

      for (int c = channels; c < buffer->getNumChannels(); ++c)
        buffer->clear(c, info.startSample, num_samples);
    }

    void releaseResources() override { }

  private:
    static constexpr int kPollMs = 25;
    static constexpr int kHotplugMs = 500;

    struct Patch {
      File file;
      json state;
    };

    // Receives all MIDI. Program Change is handled here; the rest goes to Vital's MidiManager.
    struct MidiRouter : public MidiInputCallback {
      void handleIncomingMidiMessage(MidiInput* source, const MidiMessage& message) override {
        if (message.isProgramChange()) {
          int channel = owner->config_.program_channel;
          if (channel == 0 || message.getChannel() == channel)
            owner->pending_program_.store(message.getProgramChangeNumber());
          return;
        }
        owner->midi_manager_->handleIncomingMidiMessage(source, message);
      }

      LiveSynth* owner = nullptr;
    };

    void loadPatches() {
      for (const auto& entry : config_.programs) {
        Patch patch;
        patch.file = entry.second;
        try {
          if (!patch.file.existsAsFile()) {
            live::log("Missing patch file: " + patch.file.getFullPathName());
            continue;
          }
          patch.state = json::parse(patch.file.loadFileAsString().toStdString(), nullptr);
          patches_[entry.first] = std::move(patch);
        }
        catch (const std::exception& e) {
          live::log("Cannot parse " + patch.file.getFullPathName() + ": " + e.what());
        }
      }
      live::log("Loaded " + String((int)patches_.size()) + " patches");
    }

    // Runs on the message thread (never from the MIDI or audio thread).
    bool loadProgram(int program) {
      auto it = patches_.find(program);
      if (it == patches_.end()) {
        live::log("Program " + String(program) + " is not mapped; keeping current patch");
        return false;
      }

      try {
        if (!loadFromJson(it->second.state)) {
          live::log("Patch was created with a newer Vital version: " + it->second.file.getFileName());
          return false;
        }
      }
      catch (const std::exception& e) {
        live::log("Patch load failed: " + it->second.file.getFileName() + " (" + e.what() + ")");
        return false;
      }

      active_file_ = it->second.file;
      setPresetName(it->second.file.getFileNameWithoutExtension());
      live::log("Program " + String(program) + " -> " + it->second.file.getFileName());
      return true;
    }

    bool startAudio() {
      AudioDeviceManager::AudioDeviceSetup setup;
      device_manager_.getAudioDeviceSetup(setup);
      setup.sampleRate = config_.sample_rate;
      if (config_.buffer_size > 0)
        setup.bufferSize = config_.buffer_size;
      if (config_.audio_device.isNotEmpty())
        setup.outputDeviceName = config_.audio_device;

      String error = device_manager_.initialise(0, vital::kNumChannels, nullptr, true, String(), &setup);
      if (error.isNotEmpty())
        live::log("Audio init: " + error);

      if (device_manager_.getCurrentAudioDevice() == nullptr) {
        const OwnedArray<AudioIODeviceType>& types = device_manager_.getAvailableDeviceTypes();
        for (AudioIODeviceType* type : types) {
          device_manager_.setCurrentAudioDeviceType(type->getTypeName(), true);
          if (device_manager_.getCurrentAudioDevice())
            break;
        }
      }

      AudioIODevice* device = device_manager_.getCurrentAudioDevice();
      if (device == nullptr) {
        live::log("No audio output device available");
        return false;
      }

      live::log("Audio: " + device->getName() + ", " + String(device->getCurrentSampleRate()) + " Hz, " +
                String(device->getCurrentBufferSizeSamples()) + " samples");

      audio_player_.setSource(this);
      device_manager_.addAudioCallback(&audio_player_);
      return true;
    }

    void startMidi() {
      virtual_port_ = live::wrapMidiInput(MidiInput::createNewDevice(config_.midi_port_name, &router_));
      if (virtual_port_) {
        virtual_port_->start();
        live::log("Virtual MIDI input port: " + config_.midi_port_name);
      }
      else
        live::log("Could not create a virtual MIDI port");

      device_manager_.addMidiInputCallback(String(), &router_);
      refreshMidiInputs();
    }

    void refreshMidiInputs() {
      StringArray midi_ins(MidiInput::getDevices());
      for (const String& midi_in : midi_ins) {
        if (!current_midi_ins_.contains(midi_in)) {
          device_manager_.setMidiInputEnabled(midi_in, true);
          live::log("MIDI input enabled: " + midi_in);
        }
      }
      current_midi_ins_ = midi_ins;
    }

    void timerCallback() override {
      if (live::quitFlag()) {
        stopTimer();
        MessageManager::getInstance()->stopDispatchLoop();
        return;
      }

      int program = pending_program_.exchange(-1);
      if (program >= 0)
        loadProgram(program);

      poll_ms_ += kPollMs;
      if (poll_ms_ >= kHotplugMs) {
        poll_ms_ = 0;
        refreshMidiInputs();
      }
    }

    Config config_;
    std::map<int, Patch> patches_;
    std::atomic<int> pending_program_{-1};
    AudioDeviceManager device_manager_;
    AudioSourcePlayer audio_player_;
    MidiRouter router_;
    std::unique_ptr<MidiInput> virtual_port_;
    StringArray current_midi_ins_;
    double sample_rate_ = vital::kDefaultSampleRate;
    double current_time_ = 0.0;
    int poll_ms_ = 0;
};

// Entry point for `vital --live --config <file>`.
inline int runLiveSynth(const String& config_path) {
  MessageManager::getInstance();  // the calling thread becomes the message thread

  int result = 0;
  {
    if (config_path.isEmpty()) {
      live::log("Usage: vital --live --config <config.json>");
      result = 1;
    }
    else {
      File config_file = File::getCurrentWorkingDirectory().getChildFile(config_path);
      LiveSynth::Config config;
      String error;
      if (!config_file.existsAsFile()) {
        live::log("Config not found: " + config_file.getFullPathName());
        result = 1;
      }
      else if (!LiveSynth::loadConfig(config_file, config, error)) {
        live::log(error);
        result = 1;
      }
      else {
        std::signal(SIGINT, live::signalHandler);
        std::signal(SIGTERM, live::signalHandler);

        LiveSynth synth(config);
        if (synth.start())
          MessageManager::getInstance()->runDispatchLoop();
        else
          result = 1;
      }
    }
  }

  DeletedAtShutdown::deleteAll();
  MessageManager::deleteInstance();
  return result;
}

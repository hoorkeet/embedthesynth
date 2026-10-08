// Live (non-render) mode for the headless Vital build.
// Header-only: ALSA audio, virtual ALSA MIDI port (+ optional hardware inputs),
// Program Change N -> first "<N>*.vital" file in a patch directory.
#pragma once

#include "JuceHeader.h"
#include "load_save.h"
#include "sound_engine.h"
#include "synth_base.h"
#include "synth_constants.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

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

  // Accepts "--name value" and "--name=value".
  inline bool argValue(int argc, const char* argv[], const char* name, String& value) {
    const std::string flag(name);
    const std::string prefix = flag + "=";
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == flag && i + 1 < argc) {
        value = String::fromUTF8(argv[i + 1]);
        return true;
      }
      if (arg.compare(0, prefix.size(), prefix) == 0) {
        value = String::fromUTF8(arg.c_str() + prefix.size());
        return true;
      }
    }
    return false;
  }

  // "000_bass.vital", "5 lead", "037 wah fender.vital" -> 0, 5, 37. Not a program: "1234_x", "bass".
  inline int programFromName(const String& name) {
    int digits = 0;
    while (digits < name.length() && CharacterFunctions::isDigit(name[digits]))
      ++digits;
    if (digits == 0 || digits > 3)
      return -1;
    int program = name.substring(0, digits).getIntValue();
    return program <= 127 ? program : -1;
  }
}

class LiveSynth : public HeadlessSynth, public AudioSource, private Timer {
  public:
    struct Config {
      File patch_dir;
      String midi_port_name = "Vital";
      String midi_inputs;                        // hardware MIDI: "" = none, "all", or "Akai,Keystation" (name substrings)
      String audio_device;                       // empty = system default
      double sample_rate = vital::kDefaultSampleRate;
      int buffer_size = 256;
      int default_program = 0;                   // loaded at startup; -1 = init preset
      int program_channel = 0;                   // 0 = any channel, 1-16 = only that channel
    };

    // Optional JSON config; keys: patch_dir, midi_port_name, midi_inputs, audio_device,
    // sample_rate, buffer_size, default_program, program_channel. Command line overrides it.
    static bool loadConfigFile(const File& config_file, Config& config, String& error) {
      try {
        json data = json::parse(config_file.loadFileAsString().toStdString(), nullptr);
        if (!data.is_object()) {
          error = "Config root must be a JSON object.";
          return false;
        }

        auto str = [&](const char* key, const String& fallback) {
          return data.count(key) ? String::fromUTF8(data[key].get<std::string>().c_str()) : fallback;
        };

        if (data.count("patch_dir"))
          config.patch_dir = config_file.getParentDirectory().getChildFile(str("patch_dir", String()));
        config.midi_port_name = str("midi_port_name", config.midi_port_name);
        config.midi_inputs = str("midi_inputs", config.midi_inputs);
        config.audio_device = str("audio_device", config.audio_device);
        config.sample_rate = data.value("sample_rate", config.sample_rate);
        config.buffer_size = data.value("buffer_size", config.buffer_size);
        config.default_program = data.value("default_program", config.default_program);
        config.program_channel = data.value("program_channel", config.program_channel);
      }
      catch (const std::exception& e) {
        error = String("Bad config: ") + e.what();
        return false;
      }
      return true;
    }

    static bool parseCommandLine(int argc, const char* argv[], Config& config, String& error) {
      String value;
      if (live::argValue(argc, argv, "--config", value)) {
        File config_file = File::getCurrentWorkingDirectory().getChildFile(value);
        if (!config_file.existsAsFile()) {
          error = "Config not found: " + config_file.getFullPathName();
          return false;
        }
        if (!loadConfigFile(config_file, config, error))
          return false;
      }

      if (live::argValue(argc, argv, "--patches", value))
        config.patch_dir = File::getCurrentWorkingDirectory().getChildFile(value);
      if (live::argValue(argc, argv, "--port-name", value))
        config.midi_port_name = value;
      if (live::argValue(argc, argv, "--midi-inputs", value))
        config.midi_inputs = value;
      if (live::argValue(argc, argv, "--audio-device", value))
        config.audio_device = value;
      if (live::argValue(argc, argv, "--rate", value))
        config.sample_rate = value.getDoubleValue();
      if (live::argValue(argc, argv, "--buffer", value))
        config.buffer_size = value.getIntValue();
      if (live::argValue(argc, argv, "--default-program", value))
        config.default_program = value.getIntValue();
      if (live::argValue(argc, argv, "--channel", value))
        config.program_channel = value.getIntValue();

      if (config.patch_dir == File() || !config.patch_dir.isDirectory()) {
        error = "Patch directory missing or not a directory (use --patches <dir>): " +
                config.patch_dir.getFullPathName();
        return false;
      }
      return true;
    }

    explicit LiveSynth(const Config& config) : config_(config) { router_.owner = this; }
    ~LiveSynth() override { stop(); }

    bool start() {
      live::log("Patch dir: " + config_.patch_dir.getFullPathName() + " (" + String((int)scanPatchDir(false).size()) +
                " numbered patches)");

      if (config_.default_program < 0 || !loadProgram(config_.default_program))
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

    struct CachedPatch {
      File file;
      Time modified;
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

    // program number -> file; first name (sorted) wins on duplicates.
    std::map<int, File> scanPatchDir(bool warn_duplicates = true) {
      Array<File> found;
      config_.patch_dir.findChildFiles(found, File::findFiles, false, "*.vital");
      std::vector<File> files(found.begin(), found.end());
      std::sort(files.begin(), files.end(), [](const File& a, const File& b) {
        return a.getFileName() < b.getFileName();
      });

      std::map<int, File> index;
      for (const File& file : files) {
        int program = live::programFromName(file.getFileName());
        if (program < 0)
          continue;
        if (index.count(program)) {
          if (warn_duplicates)
            live::log("Duplicate program " + String(program) + ": ignoring " + file.getFileName());
          continue;
        }
        index[program] = file;
      }
      return index;
    }

    // Runs on the message thread (never from the MIDI or audio thread).
    bool loadProgram(int program) {
      std::map<int, File> index = scanPatchDir();
      auto found = index.find(program);
      if (found == index.end()) {
        live::log("No patch numbered " + String(program) + " in " + config_.patch_dir.getFileName() +
                  "; keeping current patch");
        return false;
      }

      const File file = found->second;
      const Time modified = file.getLastModificationTime();
      CachedPatch& cached = cache_[program];
      if (cached.file != file || cached.modified != modified) {
        try {
          cached.state = json::parse(file.loadFileAsString().toStdString(), nullptr);
          cached.file = file;
          cached.modified = modified;
        }
        catch (const std::exception& e) {
          live::log("Cannot parse " + file.getFileName() + ": " + e.what());
          cache_.erase(program);
          return false;
        }
      }

      try {
        if (!loadFromJson(cached.state)) {
          live::log("Patch was created with a newer Vital version: " + file.getFileName());
          return false;
        }
      }
      catch (const std::exception& e) {
        live::log("Patch load failed: " + file.getFileName() + " (" + e.what() + ")");
        return false;
      }

      active_file_ = file;
      setPresetName(file.getFileNameWithoutExtension());
      live::log("Program " + String(program) + " -> " + file.getFileName());
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

    bool isHardwareInputAllowed(const String& name) const {
      String filter = config_.midi_inputs.trim();
      if (filter.isEmpty())
        return false;
      if (filter.equalsIgnoreCase("all"))
        return true;

      StringArray parts;
      parts.addTokens(filter, ",", "");
      for (const String& part : parts) {
        String substring = part.trim();
        if (substring.isNotEmpty() && name.containsIgnoreCase(substring))
          return true;
      }
      return false;
    }

    void refreshMidiInputs() {
      StringArray midi_ins(MidiInput::getDevices());
      for (const String& midi_in : midi_ins) {
        if (!current_midi_ins_.contains(midi_in) && isHardwareInputAllowed(midi_in)) {
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
    std::map<int, CachedPatch> cache_;
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

// Entry point for `vital --live --patches <dir> [--port-name N] [--midi-inputs S] [--config F] ...`.
inline int runLiveSynth(int argc, const char* argv[]) {
  MessageManager::getInstance();  // the calling thread becomes the message thread

  int result = 0;
  {
    LiveSynth::Config config;
    String error;
    if (!LiveSynth::parseCommandLine(argc, argv, config, error)) {
      live::log(error);
      live::log("Usage: vital --live --patches <dir> [--port-name NAME] [--midi-inputs all|Akai,..] "
                "[--audio-device NAME] [--rate HZ] [--buffer N] [--default-program N] [--channel 1-16] "
                "[--config file.json]");
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

  DeletedAtShutdown::deleteAll();
  MessageManager::deleteInstance();
  return result;
}

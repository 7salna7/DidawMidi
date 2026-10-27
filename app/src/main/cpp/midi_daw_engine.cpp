#include <jni.h>
#include <oboe/Oboe.h>
#include <android/log.h>
#include <android/amidi.h>
#include <atomic>
#include <vector>
#include <algorithm>

#define LOG_TAG "DidawMidiEngine"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define MAX_CHANNELS 16
#define BUFFER_SIZE_COEFF 2
#define DELAY_BUFFER_SIZE 48000 
#define MAX_RECORDING_SIZE 480000 

struct AudioChannel {
    const float* sampleData = nullptr;
    int32_t totalSamples = 0;
    std::atomic<int32_t> currentPosition{0};
    std::atomic<bool> isPlaying{false};
    std::atomic<float> channelVolume{1.0f};
};

class MidiDawEngine : public oboe::AudioStreamDataCallback {
public:
    MidiDawEngine() {
        mChannels.resize(MAX_CHANNELS);
        for (int i = 0; i < MAX_CHANNELS; ++i) {
            mChannels[i].isPlaying.store(false);
        }
        mDelayBufferLeft.resize(DELAY_BUFFER_SIZE, 0.0f);
        mDelayBufferRight.resize(DELAY_BUFFER_SIZE, 0.0f);
        mDelayFeedback.store(0.4f); 
        mDelayWetLevel.store(0.3f);  
        
        mRecordingBuffer.resize(MAX_RECORDING_SIZE, 0.0f);
        mIsRecording.store(false);
        mRecordingIndex.store(0);
    }

    void startStreams() {
        oboe::AudioStreamBuilder outBuilder;
        outBuilder.setDirection(oboe::Direction::Output)
                  ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
                  ->setSharingMode(oboe::SharingMode::Exclusive)
                  ->setFormat(oboe::AudioFormat::Float)
                  ->setChannelCount(oboe::ChannelCount::Stereo)
                  ->setDataCallback(this);

        oboe::Result outResult = outBuilder.openStream(mOutputStream);
        if (outResult == oboe::Result::OK && mOutputStream) {
            mOutputStream->setBufferSizeInFrames(mOutputStream->getFramesPerBurst() * BUFFER_SIZE_COEFF);
            mOutputStream->start();
        }

        oboe::AudioStreamBuilder inBuilder;
        inBuilder.setDirection(oboe::Direction::Input)
                 ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
                 ->setSharingMode(oboe::SharingMode::Exclusive)
                 ->setFormat(oboe::AudioFormat::Float)
                 ->setChannelCount(oboe::ChannelCount::Mono) 
                 ->setDataCallback(this);

        oboe::Result inResult = inBuilder.openStream(mInputStream);
        if (inResult == oboe::Result::OK && mInputStream) {
            mInputStream->start();
        }
    }

    void startRecording() {
        mRecordingIndex.store(0);
        mIsRecording.store(true);
    }

    int32_t stopRecording() {
        mIsRecording.store(false);
        return mRecordingIndex.load();
    }

    void triggerChannelSample(int channelId, const float* buffer, int32_t sampleCount, float vol) {
        if (channelId < 0 || channelId >= MAX_CHANNELS) return;
        mChannels[channelId].sampleData = buffer;
        mChannels[channelId].totalSamples = sampleCount;
        mChannels[channelId].channelVolume.store(vol);
        mChannels[channelId].currentPosition.store(0);
        mChannels[channelId].isPlaying.store(true);
    }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *audioStream, void *audioData, int32_t numFrames) override {
        if (audioStream->getDirection() == oboe::Direction::Input) {
            if (mIsRecording.load()) {
                const float *inputSamples = static_cast<const float *>(audioData);
                int32_t writeIdx = mRecordingIndex.load();
                for (int32_t i = 0; i < numFrames; ++i) {
                    if (writeIdx < MAX_RECORDING_SIZE) {
                        mRecordingBuffer[writeIdx++] = inputSamples[i];
                    }
                }
                mRecordingIndex.store(writeIdx);
            }
            return oboe::DataCallbackResult::Continue;
        }

        float *output = static_cast<float *>(audioData);
        int32_t totalChannels = audioStream->getChannelCount();
        std::fill_n(output, numFrames * totalChannels, 0.0f);

        for (int32_t frame = 0; frame < numFrames; ++frame) {
            float accumLeft = 0.0f;
            float accumRight = 0.0f;

            for (auto& chan : mChannels) {
                if (!chan.isPlaying.load()) continue;

                int32_t currentPos = chan.currentPosition.load();
                if (currentPos >= chan.totalSamples) {
                    chan.isPlaying.store(false);
                    continue;
                }

                float sample = chan.sampleData[currentPos] * chan.channelVolume.load();
                accumLeft += sample;
                accumRight += sample;
                chan.currentPosition.store(currentPos + 1);
            }

            int32_t delayedIndex = (mDelayWriteIndex + DELAY_BUFFER_SIZE - 12000) % DELAY_BUFFER_SIZE;
            float wetLeft = mDelayBufferLeft[delayedIndex] * mDelayWetLevel.load();
            float wetRight = mDelayBufferRight[delayedIndex] * mDelayWetLevel.load();

            mDelayBufferLeft[mDelayWriteIndex] = accumLeft + (mDelayBufferLeft[delayedIndex] * mDelayFeedback.load());
            mDelayBufferRight[mDelayWriteIndex] = accumRight + (mDelayBufferRight[delayedIndex] * mDelayFeedback.load());
            mDelayWriteIndex = (mDelayWriteIndex + 1) % DELAY_BUFFER_SIZE;

            output[frame * totalChannels] = accumLeft + wetLeft;
            if (totalChannels > 1) {
                output[frame * totalChannels + 1] = accumRight + wetRight;
            }
        }
        return oboe::DataCallbackResult::Continue;
    }

private:
    std::shared_ptr<oboe::AudioStream> mOutputStream;
    std::shared_ptr<oboe::AudioStream> mInputStream;
    std::vector<AudioChannel> mChannels;
    
    std::vector<float> mDelayBufferLeft;
    std::vector<float> mDelayBufferRight;
    int32_t mDelayWriteIndex = 0;
    std::atomic<float> mDelayFeedback;
    std::atomic<float> mDelayWetLevel;

    std::vector<float> mRecordingBuffer;
    std::atomic<bool> mIsRecording;
    std::atomic<int32_t> mRecordingIndex;
};

static MidiDawEngine *engineInstance = nullptr;

extern "C" JNIEXPORT void JNICALL
Java_com_webapp_didawmidi_NativeAudioEngine_nativeInit(JNIEnv *env, jobject thiz) {
    if (!engineInstance) {
        engineInstance = new MidiDawEngine();
        engineInstance->startStreams();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_webapp_didawmidi_NativeAudioEngine_nativeStartRecording(JNIEnv *env, jobject thiz) {
    if (engineInstance) engineInstance->startRecording();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_webapp_didawmidi_NativeAudioEngine_nativeStopRecording(JNIEnv *env, jobject thiz) {
    return engineInstance ? engineInstance->stopRecording() : 0;
}

extern "C" JNIEXPORT void JNICALL
Java_com_webapp_didawmidi_NativeAudioEngine_nativeTriggerPad(JNIEnv *env, jobject thiz, jint channel_id, jfloatArray pcm_array, jint size, jfloat volume) {
    if (!engineInstance) return;
    jfloat* samples = env->GetFloatArrayElements(pcm_array, nullptr);
    engineInstance->triggerChannelSample(channel_id, samples, size, volume);
    env->ReleaseFloatArrayElements(pcm_array, samples, JNI_ABORT);
}

extern "C" JNIEXPORT void JNICALL
Java_com_webapp_didawmidi_NativeAudioEngine_nativeSetAutomationVolume(JNIEnv *env, jobject thiz, jint channel_id, jfloat volume) {
    if (engineInstance) {
        engineInstance->setVolumeAutomation(channel_id, volume);
    }
}

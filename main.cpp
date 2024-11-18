#include <iostream>
#include <string>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/asio/ssl.hpp>
#include <memory>
#include <thread>
#include <chrono>
#include "picojson.h"

#include "api/create_peerconnection_factory.h"
#include "api/peer_connection_interface.h"
#include "api/data_channel_interface.h"

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"

#include "api/video/i420_buffer.h"

#include "modules/video_capture/video_capture_factory.h"

#include "opencv2/opencv.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;
using namespace webrtc;

class SDPObserver;
class SSDO;


// Create WebSocket message using JSON format
std::string createMessage(const std::string& type, const std::string& sdp_or_candidate) {
    picojson::object message;
    message["type"] = picojson::value(type);
    message["data"] = picojson::value(sdp_or_candidate);
    picojson::value v(message);
    return v.serialize();
}

// WebRTC PeerConnection Observer
class MyPeerConnectionObserver : public webrtc::PeerConnectionObserver {
public:
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override {
        std::cout << "Signaling state changed: " << new_state << std::endl;
    }

    void OnAddStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override {
      std::cout << "AddStream" << std::endl;
    };

    void OnRemoveStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override {
      std::cout << "RemoveStream" << std::endl;
    };

    void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {
        std::cout << "Data channel created: " << data_channel->label() << std::endl;
    }

    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override {
        std::cout << "ICE gathering state changed: " << new_state << std::endl;
    }

    void OnIceCandidate(const webrtc::IceCandidateInterface *candidate) override {
        std::cout << "MyPeerConnectionObserver::IceCandidate" << std::endl;
    }

    // PeerConnectionObserver implementation
    void OnAddTrack(
        rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
        const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>> &streams
    ) override {
        // this method is called when we receive track data from counterpart
        std::cout << "on add track\n";
       
    }

    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override {
        std::cout << "connection change "<< "\n";
    }
    void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {}
    void OnRemoveTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override {}
    void OnRenegotiationNeeded() override {}
    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override {}
    void OnIceConnectionReceivingChange(bool receiving) override {}
    void OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override {}
    void OnIceCandidateError(const std::string& address, int port, const std::string& url, int error_code, const std::string& error_text) override {}
};

// WebRTC DataChannel Observer
class MyDataChannelObserver : public webrtc::DataChannelObserver {
public:
    
    void OnMessage(const webrtc::DataBuffer& buffer) override {
        std::string received_message(buffer.data.data<char>(), buffer.data.size());
        std::cout << "MyDataChannelObserver Received message: " << received_message << std::endl;
    }

    void OnStateChange() override {
        if (!data_channel_) {
            std::cerr << "Data channel is not initialized or has been destroyed." << std::endl;
            return;
        }

        try {
            std::cout << "State changed: " << data_channel_->state() << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error in OnStateChange: " << e.what() << std::endl;
        }
        std::cout << "DataChannel state changed!" << std::endl;
    }


    void OnBufferedAmountChange(uint64_t previous_amount) override {
        std::cout << "Buffered amount changed. Previous: " << previous_amount << std::endl;
    }
    private:
    rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel_;
};

class WebSocketClient {
public:
    WebSocketClient(boost::asio::io_context& io_context, const std::string& host, const std::string& port)
        : io_context_(io_context), host_(host), port_(port), ws_(io_context_) {}  // Initialize ws_ directly

    void connect() {
        try {
            tcp::resolver resolver(io_context_);
            // Create WebSocket connection
            auto const results = resolver.resolve(host_, port_);
            asio::connect(ws_.next_layer(), results.begin(), results.end());

            // Perform WebSocket handshake
            ws_.handshake(host_, "/ws");

            std::cout << "WebSocket connected!" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "WebSocket connection failed: " << e.what() << std::endl;
        }
    }

    void sendMessage(const std::string& message) {
        try {
            ws_.write(asio::buffer(message));
            std::cout << "Sent WebSocket message: " << message << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error sending WebSocket message: " << e.what() << std::endl;
        }
    }

    void receiveWebSocketMessage(webrtc::PeerConnectionInterface* peer_connection, webrtc::SetSessionDescriptionObserver* ssdo_observer) 
    {
        try {
            while (1)
            {
                // Receive message from WebSocket
                beast::flat_buffer buffer;
                ws_.read(buffer);

                std::string received_message = beast::buffers_to_string(buffer.data());
                std::cout << "Received WebSocket message: " << received_message << std::endl;

                // Parse JSON message
                picojson::value v;
                std::string err = picojson::parse(v, received_message);
                if (!err.empty()) {
                    std::cerr << "Error parsing JSON message: " << err << std::endl;
                    return;
                }

                picojson::object message = v.get<picojson::object>();

                // Check for "type" field and process accordingly
                if (message.find("type") != message.end() && message["type"].is<std::string>()) {
                    std::string message_type = message["type"].get<std::string>();
                    std::string sdp_or_candidate = "";

                    if (message.find("sdp") != message.end() && message["sdp"].is<std::string>()) {
                        sdp_or_candidate = message["sdp"].get<std::string>();
                    } else {
                        std::cerr << "Error: 'sdp' is not a string!" << std::endl;
                    }

                    if (message_type == "answer") {
                        std::cout << "Received SDP Answer: " << sdp_or_candidate << std::endl;

                        // Process SDP Answer
                        webrtc::SdpParseError error;
                        auto session_description = webrtc::CreateSessionDescription("answer", sdp_or_candidate, &error);
                        if (!session_description) {
                            std::cout << "Failed to parse SDP: " << error.description << std::endl;
                            return;
                        }
                        if (session_description) {
                            if (peer_connection && peer_connection->signaling_state() == webrtc::PeerConnectionInterface::kHaveLocalOffer) {                            
                                peer_connection->SetRemoteDescription(ssdo_observer, session_description);
                            } else {
                                std::cerr << "Error: peer_connection" << std::endl;
                            }
                        } else {
                            std::cerr << "Error parsing SDP answer: " << error.description << std::endl;
                        }
                    } else if (message_type == "candidate") {
                        std::string candidate_str;

                        if (message.find("candidate") != message.end()) {
                            auto& candidate_obj = message["candidate"];
                            if (candidate_obj.is<picojson::object>()) {
                                auto sdp_mid = candidate_obj.get("sdpMid").to_str();
                                auto sdp_mline_index = static_cast<int>(candidate_obj.get("sdpMLineIndex").get<double>());
                                auto candidate_str = candidate_obj.get("candidate").to_str();

                                webrtc::SdpParseError error;
                                auto ice_candidate = webrtc::CreateIceCandidate(sdp_mid, sdp_mline_index, candidate_str, &error);
                                if (ice_candidate) {
                                    std::cout << "Received ICE Candidate: " << candidate_str << std::endl;
                                    if (peer_connection->AddIceCandidate(ice_candidate)) {
                                        std::cout << "ICE candidate added successfully!" << std::endl;
                                    } else {
                                        std::cerr << "Failed to add ICE candidate!" << std::endl;
                                    }
                                } else {
                                    std::cerr << "Error creating ICE candidate: " << error.description << std::endl;
                                }
                            } else {
                                std::cerr << "Error: 'candidate' is not an object!" << std::endl;
                            }
                        }
                    } 
                    else {
                        std::cerr << "Error: Unsupported message type: " << message_type << std::endl;
                    }
                } else {
                    std::cerr << "Error: 'type' is missing or not a string!" << std::endl;
                }
            }     
        } catch (const std::exception& e) {
            std::cerr << "Error receiving WebSocket message: " << e.what() << std::endl;
        }
    }
private:
    boost::asio::io_context& io_context_;
    std::string host_;
    std::string port_;
    beast::websocket::stream<tcp::socket> ws_;  // WebSocket stream
};

class SSDO : public webrtc::SetSessionDescriptionObserver {
   public:
        void OnSuccess() override {
            std::cout << "SetSessionDescriptionObserver::OnSuccess" << std::endl;
        };
        void OnFailure(webrtc::RTCError error) override {
        std::cout << "SetSessionDescriptionObserver::OnFailure" << std::endl
                    << error.message() << std::endl;
        };
};

class SDPObserver : public webrtc::CreateSessionDescriptionObserver {
private:
    rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_ = nullptr;
    WebSocketClient* webSocketClient_ = nullptr;
    rtc::scoped_refptr<SSDO> ssdo_ = nullptr;
public:
    explicit SDPObserver (
        rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection
        , rtc::scoped_refptr<SSDO> ssdo) : 
        peer_connection_(peer_connection), ssdo_(ssdo) {}

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
        std::string sdp;
        desc->ToString(&sdp);
        std::cout << "SDP Offer created successfully:\n" << sdp << std::endl;
        peer_connection_->SetLocalDescription(ssdo_.get(), desc);
        std::string offer_message = createMessage("offer", sdp);
        webSocketClient_->sendMessage(offer_message);  
    }

    void OnFailure(webrtc::RTCError error) override {
        std::cerr << "Failed to create SDP Offer: " << error.message() << std::endl;
    }

    void setWebSocketClient(WebSocketClient* client) {
        webSocketClient_ = client;
    }
};

class MyVideoTrackSource : public webrtc::VideoTrackSourceInterface {
public:
    rtc::VideoSinkInterface<webrtc::VideoFrame>* sink_to_write = nullptr;
    // call this method to send frame
    void sendFrame(const webrtc::VideoFrame& frame) {
        if (!sink_to_write) return;
        sink_to_write->OnFrame(frame);
    }
    // VideoTrackSourceInterface implementation
    void AddOrUpdateSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink, const rtc::VideoSinkWants& wants) override {
        sink_to_write = sink;
        std::cout << "sink updated or added to my video source\n";
    }
    void RemoveSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override {
        std::cout << "sink removed from my video source\n";
        sink_to_write = nullptr;
    }
    // just mock implementation of the rest of the methods
    webrtc::MediaSourceInterface::SourceState state() const override {
        return webrtc::MediaSourceInterface::SourceState::kLive;
    }
    bool remote() const override { return false; }
    bool is_screencast() const override { return false; }
    std::optional<bool> needs_denoising() const override { return std::nullopt; }

    bool GetStats(Stats* stats) override { return false; }
    bool SupportsEncodedOutput() const override { return false; }
    void GenerateKeyFrame() override {}
    void AddEncodedSink(rtc::VideoSinkInterface<webrtc::RecordableEncodedFrame>* sink) override {}
    void RemoveEncodedSink(rtc::VideoSinkInterface<webrtc::RecordableEncodedFrame>* sink) override {}
    // probably need to gather and notify these observers on state changes
    void RegisterObserver(webrtc::ObserverInterface* observer) override {}
    void UnregisterObserver(webrtc::ObserverInterface* observer) override {}
};

class MyAudioSource : public webrtc::AudioSourceInterface {
public:
    webrtc::AudioTrackSinkInterface* sink_to_write = nullptr;
    // call this method to send audio data
    void sendAudioData(const void* audio_data,
                      int bits_per_sample,
                      int sample_rate,
                      size_t number_of_channels,
                      size_t number_of_frames) {
        if (!sink_to_write) return;
        sink_to_write->OnData(audio_data, bits_per_sample, sample_rate, number_of_channels, number_of_frames);
    }
    // AudioSourceInterface implementation
    void AddSink(webrtc::AudioTrackSinkInterface* sink) override {
        sink_to_write = sink;
        std::cout << "sink added to my audio source\n";
    }
    void RemoveSink(webrtc::AudioTrackSinkInterface* sink) override {
        sink_to_write = nullptr;
        std::cout << "sink removed from my audio source\n";
    }
    const cricket::AudioOptions options() const override {
        return cricket::AudioOptions();
    }
    webrtc::MediaSourceInterface::SourceState state() const override {
        return webrtc::MediaSourceInterface::SourceState::kLive;
    }
    bool remote() const override { return false; }
    // probably need to gather and notify these observers on state changes
    void RegisterObserver(webrtc::ObserverInterface* observer) override {}
    void UnregisterObserver(webrtc::ObserverInterface* observer) override {}
};

// OpenCV-based video capturer
class OpenCVVideoCapturer {
public:
    OpenCVVideoCapturer(int device_id = 0)
        : capture_(device_id) {
        if (!capture_.isOpened()) {
            RTC_LOG(LS_ERROR) << "Failed to open video capture device!";
        }
    }

    ~OpenCVVideoCapturer() {
        if (capture_.isOpened()) {
            capture_.release();
        }
    }

    bool Start(webrtc::VideoCaptureCapability& capability) {
        capability.width = 640;
        capability.height = 480;
        capture_.set(cv::CAP_PROP_FRAME_WIDTH, capability.width);
        capture_.set(cv::CAP_PROP_FRAME_HEIGHT, capability.height);
        capture_.set(cv::CAP_PROP_FPS, 30);  // Assuming FPS of 30 for now

        return capture_.isOpened();
    }

    void CaptureFrame(std::function<void(const webrtc::VideoFrame&)> callback) {
        if (!capture_.isOpened()) {
            RTC_LOG(LS_ERROR) << "Capture device is not opened!";
            return;
        }

        cv::Mat frame;
        capture_ >> frame;

        if (frame.empty()) {
            RTC_LOG(LS_WARNING) << "Captured empty frame!";
            return;
        }

        // Convert the OpenCV frame (cv::Mat) to I420 format for WebRTC
        rtc::scoped_refptr<webrtc::I420Buffer> i420_buffer = ConvertToI420(frame);

        // Create a VideoFrame from the I420Buffer, with timestamp and other metadata
        int64_t timestamp = rtc::TimeMicros();  // Or use any custom timestamp
        webrtc::VideoFrame video_frame = webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(i420_buffer)
            .set_timestamp_us(timestamp)
            .build();

        // Send the VideoFrame to the callback
        if (callback) {
            callback(video_frame);
        }
    }

private:
    cv::VideoCapture capture_;

    rtc::scoped_refptr<webrtc::I420Buffer> ConvertToI420(const cv::Mat& frame) {
        // Create an I420Buffer directly from OpenCV (cv::Mat) without manual YUV conversion
        rtc::scoped_refptr<webrtc::I420Buffer> i420_buffer = webrtc::I420Buffer::Create(frame.cols, frame.rows);

        // Convert the frame from BGR (OpenCV) to I420 (WebRTC)
        cv::Mat yuv_frame(frame.rows + frame.rows / 2, frame.cols, CV_8UC1);  // Create YUV buffer
        cv::cvtColor(frame, yuv_frame, cv::COLOR_BGR2YUV_I420);

        // Copy data to I420Buffer
        uint8_t* y_plane = i420_buffer->MutableDataY();
        uint8_t* u_plane = i420_buffer->MutableDataU();
        uint8_t* v_plane = i420_buffer->MutableDataV();

        int y_size = frame.cols * frame.rows;
        int uv_size = (frame.cols / 2) * (frame.rows / 2);

        std::memcpy(y_plane, yuv_frame.data, y_size);
        std::memcpy(u_plane, yuv_frame.data + y_size, uv_size);
        std::memcpy(v_plane, yuv_frame.data + y_size + uv_size, uv_size);

        return i420_buffer;
    }
};

// Capture loop: Captures video frames at a specified FPS
void CaptureLoop(OpenCVVideoCapturer* videoCapturer, MyVideoTrackSource* videoSource) {
    webrtc::VideoCaptureCapability capability;
    capability.width = 640;
    capability.height = 480;

    if (!videoCapturer->Start(capability)) {
        RTC_LOG(LS_ERROR) << "Failed to start video capturer!";
        return;
    }

    // Capture and send frames every 33ms (~30 FPS)
    while (true) {
        videoCapturer->CaptureFrame([videoSource](const webrtc::VideoFrame& frame) {
            videoSource->sendFrame(frame);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(33));  // Simulate 30 FPS
    }
}

void sendMessageToPeer(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel, const std::string& message) {
    if (data_channel->state() == webrtc::DataChannelInterface::kOpen) {
        webrtc::DataBuffer buffer(rtc::CopyOnWriteBuffer(message.c_str(), message.length()), true);
        data_channel->Send(buffer);
        std::cout << "Message sent: " << message << std::endl;
    } else {
        std::cerr << "DataChannel is not open. Cannot send message!" << std::endl;
    }
}

std::mutex input_mutex;
std::thread userInputThread(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
    return std::thread([data_channel]() {
        while (true) {
            std::lock_guard<std::mutex> lock(input_mutex);
            std::string user_message;
            std::cout << "Enter message to send: ";
            std::getline(std::cin, user_message);
            if (user_message == "exit") break;
            sendMessageToPeer(data_channel, user_message);
        }
    });
}

int main() {
    try {

        std::unique_ptr<rtc::Thread> signaling_thread = nullptr;
        std::unique_ptr<rtc::Thread> network_thread = nullptr;
        std::unique_ptr<rtc::Thread> worker_thread = nullptr;

        network_thread = rtc::Thread::CreateWithSocketServer();
        network_thread->SetName("network_thread", nullptr);
        network_thread->Start();

        signaling_thread = rtc::Thread::CreateWithSocketServer();
        signaling_thread->SetName("signaling_thread", nullptr);
        signaling_thread->Start();
        RTC_DCHECK(!signaling_thread->IsCurrent());

        worker_thread = rtc::Thread::Create();
        worker_thread->SetName("worker_thread", nullptr);
        worker_thread->Start();

        rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peer_connection_factory = webrtc::CreatePeerConnectionFactory(
            network_thread.get(), worker_thread.get(), signaling_thread.get(),
            nullptr, 
            webrtc::CreateBuiltinAudioEncoderFactory(), webrtc::CreateBuiltinAudioDecoderFactory(),
            webrtc::CreateBuiltinVideoEncoderFactory(), webrtc::CreateBuiltinVideoDecoderFactory(),
            nullptr, nullptr);

            RTC_DCHECK(peer_connection_factory);

        if (!peer_connection_factory) {
            std::cerr << "Failed to create PeerConnectionFactory!" << std::endl;
            return -1;
        }

        // // Create PeerConnection
        webrtc::PeerConnectionInterface::RTCConfiguration config;
        config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;  // Set UnifiedPlan for SDP semantics
        
        webrtc::PeerConnectionInterface::IceServer stun_server;
        stun_server.uri = "stun:stun.l.google.com:19302";  // STUN server URI
        config.servers.push_back(stun_server);

        std::unique_ptr<MyPeerConnectionObserver> observer = std::make_unique<MyPeerConnectionObserver>();
        rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection = peer_connection_factory->CreatePeerConnection(config, nullptr, nullptr, observer.get());
        if (!peer_connection) {
            std::cerr << "Failed to create PeerConnection!" << std::endl;
            return -1;
        }

        rtc::scoped_refptr<MyVideoTrackSource> videoSource = new rtc::RefCountedObject<MyVideoTrackSource>();
        rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track = peer_connection_factory->CreateVideoTrack("video_label", videoSource.get());

        if(!video_track) {
            std::cout << "video_track is null" <<std::endl;
        }

        auto result_or_error = peer_connection->AddTrack(video_track, {"stream_label"});
        if (!result_or_error.ok()) {
            std::cerr << "Failed to add video track to PeerConnection. Error: " << result_or_error.error().message() << std::endl;
        } else {
            std::cout << "Successfully added video track to PeerConnection." << std::endl;
        }
    
        // Create Audio Source and Track
        rtc::scoped_refptr<MyAudioSource> audioSource = new rtc::RefCountedObject<MyAudioSource>();
        rtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track = peer_connection_factory->CreateAudioTrack("audio_label", audioSource.get());

        if(!audio_track) {
            std::cout << "audio_track is null" <<std::endl;
        }

        result_or_error = peer_connection->AddTrack(audio_track, {"stream_label"});
        if (!result_or_error.ok()) {
            std::cerr << "Failed to add audio track to PeerConnection. Error: " << result_or_error.error().message() << std::endl;
        } else {
            std::cout << "Successfully added audio track to PeerConnection." << std::endl;
        }

        // Create DataChannel
        webrtc::DataChannelInit config_data_channel;
        rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel = peer_connection->CreateDataChannel("data_channel", &config_data_channel);

        // Register DataChannel Observer
        std::unique_ptr<MyDataChannelObserver> data_channel_observer = std::make_unique<MyDataChannelObserver>();
        data_channel->RegisterObserver(data_channel_observer.get());

        const std::string host = "localhost"; 
        const std::string port = "8081";     
        boost::asio::io_context io_context;
        WebSocketClient* webSocketClient_ = new WebSocketClient(io_context, host, port);
        webSocketClient_->connect();
        rtc::scoped_refptr<SSDO> ssdo = new rtc::RefCountedObject<SSDO>();
        rtc::scoped_refptr<SDPObserver> sdp_observer = 
        new rtc::RefCountedObject<SDPObserver>(peer_connection, ssdo);
        sdp_observer->setWebSocketClient(webSocketClient_);

        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        peer_connection->CreateOffer(sdp_observer.get(), options);

        std::unique_ptr<OpenCVVideoCapturer> capturer = std::make_unique<OpenCVVideoCapturer>();

        std::thread ws_thread(&WebSocketClient::receiveWebSocketMessage, webSocketClient_, peer_connection.get(), ssdo.get());
        std::thread capture_thread(CaptureLoop, capturer.get(), videoSource.get());
        std::thread input_thread = userInputThread(data_channel);
        
        input_thread.join();
        capture_thread.join();
        ws_thread.join();
    } catch (const std::exception& e) {
        std::cerr << "Error in main: " << e.what() << std::endl;
        return -1;
    }
    return 0;
}


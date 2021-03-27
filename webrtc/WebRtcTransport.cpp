#include "WebRtcTransport.h"
#include <iostream>
#include "Rtcp/Rtcp.h"

WebRtcTransport::WebRtcTransport() {
    static onceToken token([](){
        RTC::DtlsTransport::ClassInit();
    });

    dtls_transport_ = std::make_shared<RTC::DtlsTransport>(EventPollerPool::Instance().getFirstPoller(), this);
    ice_server_ = std::make_shared<RTC::IceServer>(this, makeRandStr(4), makeRandStr(24));
}

WebRtcTransport::~WebRtcTransport() {
    dtls_transport_ = nullptr;
    ice_server_ = nullptr;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void WebRtcTransport::OnIceServerSendStunPacket(const RTC::IceServer *iceServer, const RTC::StunPacket *packet, RTC::TransportTuple *tuple) {
    onWrite((char *)packet->GetData(), packet->GetSize(), (struct sockaddr_in *)tuple);
}

void WebRtcTransport::OnIceServerSelectedTuple(const RTC::IceServer *iceServer, RTC::TransportTuple *tuple) {
    InfoL;
}

void WebRtcTransport::OnIceServerConnected(const RTC::IceServer *iceServer) {
    InfoL;
    dtls_transport_->Run(RTC::DtlsTransport::Role::SERVER);
}

void WebRtcTransport::OnIceServerCompleted(const RTC::IceServer *iceServer) {
    InfoL;
}

void WebRtcTransport::OnIceServerDisconnected(const RTC::IceServer *iceServer) {
    InfoL;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void WebRtcTransport::OnDtlsTransportConnected(
        const RTC::DtlsTransport *dtlsTransport,
        RTC::SrtpSession::CryptoSuite srtpCryptoSuite,
        uint8_t *srtpLocalKey,
        size_t srtpLocalKeyLen,
        uint8_t *srtpRemoteKey,
        size_t srtpRemoteKeyLen,
        std::string &remoteCert) {
    InfoL;
    srtp_session_ = std::make_shared<RTC::SrtpSession>(RTC::SrtpSession::Type::OUTBOUND, srtpCryptoSuite, srtpLocalKey, srtpLocalKeyLen);
    onDtlsConnected();
}

void WebRtcTransport::OnDtlsTransportSendData(const RTC::DtlsTransport *dtlsTransport, const uint8_t *data, size_t len) {
    onWrite((char *)data, len);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void WebRtcTransport::onWrite(const char *buf, size_t len){
    auto tuple = ice_server_->GetSelectedTuple();
    assert(tuple);
    onWrite(buf, len, (struct sockaddr_in *)tuple);
}

std::string WebRtcTransport::GetLocalSdp() {
    RTC::DtlsTransport::Fingerprint remote_fingerprint;
    remote_fingerprint.algorithm = RTC::DtlsTransport::GetFingerprintAlgorithm("sha-256");
    remote_fingerprint.value = "";
    dtls_transport_->SetRemoteFingerprint(remote_fingerprint);

    string finger_print_sha256;
    auto finger_prints = dtls_transport_->GetLocalFingerprints();
    for (size_t i = 0; i < finger_prints.size(); i++) {
        if (finger_prints[i].algorithm == RTC::DtlsTransport::FingerprintAlgorithm::SHA256) {
            finger_print_sha256 = finger_prints[i].value;
        }
    }

    char sdp[1024 * 10] = {0};
    auto ssrc = getSSRC();
    auto ip = getIP();
    auto pt = getPayloadType();
    auto port = getPort();
    sprintf(sdp,
            "v=0\r\n"
            "o=- 1495799811084970 1495799811084970 IN IP4 %s\r\n"
            "s=Streaming Test\r\n"
            "t=0 0\r\n"
            "a=group:BUNDLE video\r\n"
            "a=msid-semantic: WMS janus\r\n"
            "m=video %u RTP/SAVPF %u\r\n"
            "c=IN IP4 %s\r\n"
            "a=mid:video\r\n"
            "a=sendonly\r\n"
            "a=rtcp-mux\r\n"
            "a=ice-ufrag:%s\r\n"
            "a=ice-pwd:%s\r\n"
            "a=ice-options:trickle\r\n"
            "a=fingerprint:sha-256 %s\r\n"
            "a=setup:actpass\r\n"
            "a=connection:new\r\n"
            "a=rtpmap:%u H264/90000\r\n"
            "a=ssrc:%u cname:janusvideo\r\n"
            "a=ssrc:%u msid:janus janusv0\r\n"
            "a=ssrc:%u mslabel:janus\r\n"
            "a=ssrc:%u label:janusv0\r\n"
            "a=candidate:%s 1 udp %u %s %u typ %s\r\n",
            ip.c_str(), port, pt, ip.c_str(),
            ice_server_->GetUsernameFragment().c_str(),ice_server_->GetPassword().c_str(),
            finger_print_sha256.c_str(),  pt, ssrc, ssrc, ssrc, ssrc, "4", ssrc, ip.c_str(), port, "host");
    return sdp;
}

bool is_dtls(char *buf) {
    return ((*buf > 19) && (*buf < 64));
}

bool is_rtp(char *buf) {
    RtpHeader *header = (RtpHeader *) buf;
    return ((header->pt < 64) || (header->pt >= 96));
}

bool is_rtcp(char *buf) {
    RtpHeader *header = (RtpHeader *) buf;
    return ((header->pt >= 64) && (header->pt < 96));
}

void WebRtcTransport::OnInputDataPacket(char *buf, size_t len, RTC::TransportTuple *tuple) {
    if (RTC::StunPacket::IsStun((const uint8_t *) buf, len)) {
        RTC::StunPacket *packet = RTC::StunPacket::Parse((const uint8_t *) buf, len);
        if (packet == nullptr) {
            WarnL << "parse stun error" << std::endl;
            return;
        }
        ice_server_->ProcessStunPacket(packet, tuple);
        return;
    }
    if (is_dtls(buf)) {
        dtls_transport_->ProcessDtlsData((uint8_t *)buf, len);
        return;
    }
    if (is_rtp(buf)) {
        RtpHeader *header = (RtpHeader *) buf;
//        InfoL << "rtp:" << header->dumpString(len);
        return;
    }
    if (is_rtcp(buf)) {
        RtcpHeader *header = (RtcpHeader *) buf;
//        InfoL << "rtcp:" << header->dumpString();
        return;
    }
}

void WebRtcTransport::WritRtpPacket(char *buf, size_t len) {
    const uint8_t *p = (uint8_t *) buf;
    bool ret = false;
    if (srtp_session_) {
        ret = srtp_session_->EncryptRtp(&p, &len);
    }
    if (ret) {
        onWrite((char *) p, len);
    }
}

///////////////////////////////////////////////////////////////////////////////////

WebRtcTransportImp::WebRtcTransportImp(const EventPoller::Ptr &poller) {
    _socket = Socket::createSocket(poller, false);
    //随机端口，绑定全部网卡
    _socket->bindUdpSock(0);
    _socket->setOnRead([this](const Buffer::Ptr &buf, struct sockaddr *addr, int addr_len) mutable {
        OnInputDataPacket(buf->data(), buf->size(), addr);
    });
}

void WebRtcTransportImp::attach(const RtspMediaSource::Ptr &src) {
    assert(src);
    _src = src;
}

void WebRtcTransportImp::onDtlsConnected() {
    _reader = _src->getRing()->attach(_socket->getPoller(), true);
    weak_ptr<WebRtcTransportImp> weak_self = shared_from_this();
    _reader->setReadCB([weak_self](const RtspMediaSource::RingDataType &pkt){
        auto strongSelf = weak_self.lock();
        if (!strongSelf) {
            return;
        }
        pkt->for_each([&](const RtpPacket::Ptr &rtp) {
            if(rtp->type == TrackVideo) {
                //目前只支持视频
                strongSelf->WritRtpPacket(rtp->data() + RtpPacket::kRtpTcpHeaderSize,
                                          rtp->size() - RtpPacket::kRtpTcpHeaderSize);
            }
        });
    });
}

void WebRtcTransportImp::onWrite(const char *buf, size_t len, struct sockaddr_in *dst) {
    auto ptr = BufferRaw::create();
    ptr->assign(buf, len);
    _socket->send(ptr, (struct sockaddr *)(dst), sizeof(struct sockaddr));
}

uint32_t WebRtcTransportImp::getSSRC() const {
    return _src->getSsrc(TrackVideo);
}

int WebRtcTransportImp::getPayloadType() const{
    auto sdp = SdpParser(_src->getSdp());
    auto track = sdp.getTrack(TrackVideo);
    assert(track);
    return track ? track->_pt : 0;
}

uint16_t WebRtcTransportImp::getPort() const {
    //todo udp端口号应该与外网映射端口相同
    return _socket->get_local_port();
}

std::string WebRtcTransportImp::getIP() const {
    //todo 替换为外网ip
    return SockUtil::get_local_ip();
}

///////////////////////////////////////////////////////////////////




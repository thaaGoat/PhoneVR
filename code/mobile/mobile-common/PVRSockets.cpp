#include "PVRSockets.h"

//using namespace PVR;

namespace {
    TCPTalker *talker = nullptr;
    io_service *videoSvc = nullptr;
    std::thread *strThr = nullptr;
    io_service *annSvc = nullptr;
    string pcIP;
    mutex delMtx;

    bool announcing = false;

    TimeBomb headerBomb(seconds(5),
                        [] { pvrState = PVR_STATE_SHUTDOWN; }); // todo: error handling instead of shutdown

    //           pts       buf
    queue<pair<int64_t, vector<float>>> quatQueue;

    queue<EmptyVidBuf> emptyVBufs;
    queue<FilledVidBuf> filledVBufs;

    float fpsStreamRecver = 0.0;
}

extern float fpsStreamDecoder = 0.0;
extern float fpsRenderer = 0.0;

vector<float> DequeueQuatAtPts(int64_t pts) {

    vector<float> quat;
    try {
        while (quatQueue.size() > 0 && quatQueue.front().first <= pts) {
            quat = quatQueue.front().second;
            quatQueue.pop();
        }

    }
    catch(exception e) {
        PVR_DB_I("PVRSockets_DequeueQuatAtPts:: Caught Exception: " + string(e.what()));
    }

    return quat;
}

void SendAdditionalData(vector<uint16_t> maxSize, vector<float> fov, float ipd) {
    try {
        if (talker) {
            vector<uint8_t> v(2 * 2 + 4 * 4 + 4);
            memcpy(&v[0], &maxSize[0], 2 * 2);
            memcpy(&v[2 * 2], &fov[0], 4 * 4);
            memcpy(&v[2 * 2 + 4 * 4], &ipd, 4);
            if (!talker->send(PVR_MSG::ADDITIONAL_DATA, v)) PVR_DB_I(
                    "[PVRSockets::SendAdditionalData] Failed to send AddData");

            headerBomb.ignite(false);
        }
    }
    catch(exception e) {
        PVR_DB_I("PVRSockets_SendAdditionalData:: Caught Exception: " + string(e.what()));
    }
}

void PVRStartAnnouncer(const char *ip, uint16_t port, void(*segueCb)(),
                       void(*headerCb)(uint8_t *, size_t), void(*unwindSegue)()) {
    try {
        pcIP = ip; //ip will become invalid afterwards, so I capture a string copy
        std::thread([=] {
            try {
                if (talker) {
                    talker->send(PVR_MSG::DISCONNECT);
                    delete talker;
                }
                talker = new TCPTalker(port, [=](PVR_MSG msgType, vector<uint8_t> data) {
                    if (msgType == PVR_MSG::PAIR_ACCEPT) {
                        PVRStopAnnouncer();
                        if ( pcIP.length() == 0 ){
                            pcIP = talker->getIP(); // Set pcIP from addr only if pcIP is empty (override not set in android app settings)
                        } else
                        {
                            PVR_DB_I("[PVRSockets::PVRStartAnnouncer] TCPTalker, got pcIP from network as " +
                                             to_string(talker->getIP()) + " but using override ip from settings(" + pcIP + ")");
                        }
                        segueCb();
                    } else if (msgType == PVR_MSG::HEADER_NALS) {
                        headerCb(&data[0], data.size());
                        headerBomb.defuse();
                    } else if (msgType == PVR_MSG::DISCONNECT) {
                        unwindSegue();
                    }
                }, [](std::error_code err) {
                    PVR_DB_I("[PVRSockets::PVRStartAnnouncer] TCPTalker error: " + err.message());
                }, true, pcIP);

                io_service svc;
                udp::socket skt(svc);

                skt.open(udp::v4());

                skt.set_option(socket_base::broadcast(true));

                uint8_t buf[8] = {'p', 'v', 'r', PVR_MSG::PAIR_HMD};
                auto vers = PVR_CLIENT_VERSION;
                memcpy(&buf[4], &vers, 4);

                announcing = true;
//#if defined _DEBUG
                PrintNetworkInterfaceInfos();
//#endif
                PVRAnnounceToAllInterfaces(skt, buf, port);
            }
            catch (exception &e) {
                PVR_DB_I(
                        "PVRSockets_PVRStartAnnouncer::NewThread caught Exception: " + to_string(e.what()));
            }
        }).detach();
    }
    catch(exception e) {
        PVR_DB_I("PVRSockets_PVRStartAnnouncer:: Caught Exception: " + string(e.what()));
    }
}

void
PVRAnnounceToAllInterfaces(udp::socket &skt, uint8_t *buf, const uint16_t &port) {
    struct ifaddrs *ifap;

    try {
        if (pcIP.empty() && (getifaddrs(&ifap) == 0)) {

            struct ifaddrs *p = ifap;
            struct ifaddrs *start = ifap;
            while (announcing) {
                while (p) {
                    static uint32 ifaAddr, dstAddr;

                    ifaAddr = SockAddrToUint32(p->ifa_addr);
                    dstAddr = ifaAddr | ~(SockAddrToUint32(p->ifa_netmask));

                    if (ifaAddr > 0) {
                        static char ifaAddrStr[32], dstAddrStr[32];
                        ifaAddrStr[0] = '\0';
                        dstAddrStr[0] = '\0';

                        Inet_NtoA(ifaAddr, ifaAddrStr);
                        /// Found interface:  name=[lo] address=[127.0.0.1] netmask=[255.0.0.0] broadcastAddr=[127.0.0.1]
                        if (!strcmp(ifaAddrStr, "127.0.0.1"))
                        {
                            p = p->ifa_next;
                            continue;
                        }

                        Inet_NtoA(dstAddr, dstAddrStr);

                        skt.send_to(buffer(buf, 8),
                                    {address::from_string(to_string(dstAddrStr)), port});
                        static asio::error_code ec;
                        ec = asio::error_code();

                        if (ec.value()) {
                            PVR_DB_I(
                                    "[PVRSockets::PVRStartAnnouncer::PVRAnnounceToAllInterfaces(nIntf)] Announcer:send_to() Error(" +
                                    to_string(ec.value()) + ") for intf " +
                                    " name=[" + string(p->ifa_name) +
                                    "] address=[" + ifaAddrStr +
                                    "] broadcastAddr=[" + dstAddrStr + "]" +
                                    ": " + ec.message());
                        }
                    }
                    p = p->ifa_next;
                }
                usleep(500000); // 500ms
                p = start;
            }
            freeifaddrs(ifap);
        } else {
            udp::endpoint remEP;

            if(pcIP.empty()) {
                PVR_DB_I(
                        "[PVRSockets::PVRStartAnnouncer::PVRAnnounceToAllInterfaces] Could not get network interfaces. Broadcasting to immediate devices...");
                remEP = {address_v4::broadcast(), port};
            }
            else {
                PVR_DB_I(
                        "[PVRSockets::PVRStartAnnouncer::PVRAnnounceToAllInterfaces] pcIP Supplied: " + pcIP);
                remEP = {address::from_string(pcIP), port};
            }

            while (announcing) {
                skt.send_to(buffer(buf, 8), remEP);

                static asio::error_code ec;
                ec = asio::error_code();

                if (ec.value()) {
                    PVR_DB_I("[PVRSockets::PVRStartAnnouncer::PVRAnnounceToAllInterfaces] Announcer:send_to() Error(" +
                            to_string(ec.value()) + "): " + ec.message());
                }
                usleep(10000); // 10ms
            }
        }
        PVR_DB_I(
                "[PVRSockets::PVRStartAnnouncer::PVRAnnounceToAllInterfaces] Announcer Stopped.");
    }
    catch (exception &e) {
        PVR_DB_I("[PVRSockets::PVRStartAnnouncer::PVRAnnounceToAllInterfaces] caught Exception: " +
                 to_string(e.what()));
    }
}

void PrintNetworkInterfaceInfos() {
    try {
        struct ifaddrs *ifap;
        if (getifaddrs(&ifap) == 0) {
            struct ifaddrs *p = ifap;
            while (p) {
                uint32 ifaAddr = SockAddrToUint32(p->ifa_addr);
                uint32 maskAddr = SockAddrToUint32(p->ifa_netmask);
                uint32 dstAddr = ifaAddr | ~maskAddr;

                if (ifaAddr > 0) {
                    char ifaAddrStr[32];
                    Inet_NtoA(ifaAddr, ifaAddrStr);
                    char maskAddrStr[32];
                    Inet_NtoA(maskAddr, maskAddrStr);
                    char dstAddrStr[32];
                    Inet_NtoA(dstAddr, dstAddrStr);

                    PVR_DB_I("[Ancr]      Found interface:  name=[" + string(p->ifa_name) +
                             "] desc=[unavailable] address=[" + ifaAddrStr +
                             "] netmask=[" + maskAddrStr +
                             "] broadcastAddr=[" + dstAddrStr + "]");
                }
                p = p->ifa_next;
            }
            freeifaddrs(ifap);
        }
    }
    catch(exception e) {
        PVR_DB_I("PVRSockets_PrintNetworkInterfaceInfos:: Caught Exception: " + string(e.what()));
    }
}

void PVRStopAnnouncer() {
    announcing = false;
    PVR_DB_I("[PVRSockets::PVRStopAnnouncer] Stopping Announcer.");
}

void PVRStartSendSensorData(uint16_t port, bool(*getSensorData)(float *, float *)) {
    try {
        PVR_DB_I("[PVRStartSendSensorData] sending sensor data on UDP port : " + to_string(port) + ", ip: " +
                         to_string(pcIP));
        std::thread([=] {
            try {
                io_service svc;
                udp::socket skt(svc);
                udp::endpoint ep(address::from_string(pcIP), port);
                skt.open(udp::v4());

                RefWhistle ref(microseconds(8333)); // this scans loops of exactly 120 fps

                uint8_t buf[36];
                auto orQuat = reinterpret_cast<float *>(&buf[0]);
                auto acc = reinterpret_cast<float *>(&buf[4 * 4]);
                auto tm = reinterpret_cast<long long *>(&buf[4 * 4 + 3 * 4]);
                while (pvrState != PVR_STATE_SHUTDOWN) {
                    if (getSensorData(orQuat, acc)) {
                        *tm = Clk::now().time_since_epoch().count();
                        skt.send_to(buffer(buf, 36), ep);
                    }
                    ref.wait();
                }
            }
            catch (exception &e) {
                PVR_DB_I("[PVRStartSendSensorData]::NewThread caught Exception: " + to_string(e.what()));
            }
        }).detach();
    }
    catch (exception &e) {
        PVR_DB_I("[PVRStartSendSensorData] caught Exception: " + to_string(e.what()));
    }
}

bool PVRIsVidBufNeeded() {
    return emptyVBufs.size() < 3;
}

void PVREnqueueVideoBuf(EmptyVidBuf eBuf) {
    try {
        emptyVBufs.push(eBuf);
    }
    catch(exception e) {
        PVR_DB_I("PVRSockets_PVREnqueueVideoBuf:: Caught Exception: " + string(e.what()));
    }
}

FilledVidBuf PVRPopVideoBuf() {
    try {
        if (!filledVBufs.empty()) {
            auto fbuf = filledVBufs.front();
            filledVBufs.pop();
            return fbuf;
        }
    }
    catch(exception e) {
        PVR_DB_I("PVRSockets_PVRPopVideoBuf:: Caught Exception: " + string(e.what()));
    }
    return {-1, 0, 0}; //idx == -1 -> no buffers available
}

void PVRStartReceiveStreams(uint16_t port) {
    try {
        while (pvrState == PVR_STATE_SHUTDOWN)
            usleep(10000);
        PVR_DB_I("[PVRSockets::PVRStartReceiveStreams] th started.. @p:" + to_string(port));
        strThr = new std::thread([=] {
            try {
                io_service svc;
                videoSvc = &svc;
                //udp::socket skt(svc, {udp::v4(), port});
                tcp::socket skt(svc);
                asio::error_code ec = error::fault;
                PVR_DB_I("[StreamReceiver th] Connecting to pcIP " + pcIP + ":" +
                                 to_string(port));
                // TODO: Add max retries or timeout, connect will block until connected
                while (ec.value() != 0 && pvrState != PVR_STATE_SHUTDOWN)
                    skt.connect({address::from_string(pcIP), port}, ec);

                PVR_DB_I("[StreamReceiver th] socket connected pcIP " + pcIP + ":" +
                       to_string(port));

                function<void(const asio::error_code &, size_t)> handler = [&](
                        const asio::error_code &err, size_t) { ec = err; };

                uint8_t extraBuf[8 + 16 + 4 + 20 + 8 + 8];
                auto pts = reinterpret_cast<int64_t *>(extraBuf);              // these values are automatically updated
                auto quatBuf = reinterpret_cast<float *>(extraBuf +
                                                         8);        // when extraBuf is updated
                auto pktSz = reinterpret_cast<uint32_t *>(extraBuf + 8 + 16);
                auto fpsBuf = reinterpret_cast<float *>(extraBuf + 8 + 16 + 4);
                auto ctdBuf = reinterpret_cast<float *>(extraBuf + 8 + 16 + 4 + 20);
                auto timestamp = reinterpret_cast<int64_t *>(extraBuf + 8 + 16 + 4 + 20 + 8);

                // reinit queues
                quatQueue = queue<pair<int64_t, vector<float>>>();
                emptyVBufs = queue<EmptyVidBuf>();
                filledVBufs = queue<FilledVidBuf>();

                while (pvrState != PVR_STATE_SHUTDOWN) {

                    static Clk::time_point oldtime = Clk::now();

                    async_read(skt, buffer(extraBuf, sizeof(extraBuf)), handler);
                    svc.run();
                    svc.reset();

                    auto networkDelay = (int) ((duration_cast<microseconds>(
                            system_clock::now().time_since_epoch()).count() - *timestamp) / 1000);

                    PVR_DB("[StreamReceiver th] recvd 28maxBs with Error: " +
                           to_string(ec.value()) +
                           ", pts: " + to_string(*pts) + ", pktSz" + to_string(*pktSz));

                    if (ec.value() == 0) {
                        quatQueue.push({*pts, vector<float>(quatBuf, quatBuf + 4)});

                        while (emptyVBufs.empty() && pvrState != PVR_STATE_SHUTDOWN)
                            usleep(2000); //1ms

                        if (!emptyVBufs.empty()) {
                            auto eBuf = emptyVBufs.front();
                            async_read(skt, buffer(eBuf.buf, *pktSz), handler);
                            svc.run();
                            svc.reset();

                            PVR_DB("[StreamReceiver th] emptyVBufs.size: " +
                                   to_string(emptyVBufs.size()) + ", Reading sock for " +
                                   to_string(*pktSz) + "Bs");
                            if (ec.value() == 0) {
                                filledVBufs.push({eBuf.idx, *pktSz, (uint64_t) *pts});
                                PVR_DB("[StreamReceiver th] pushing onto filledVBufs idx: " +
                                       to_string(eBuf.idx) + ", size: " + to_string(*pktSz) +
                                       ", pts:" +
                                       to_string(*pts) + "...pop eVbuf ");
                                emptyVBufs.pop();
                            } else
                                break;
                        }
                    } else
                        break;

                    //PVR_DB_I("Time: "+ to_string( (duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() - *timestamp) ));
                    fpsStreamRecver = (1000000000.0 / (Clk::now() - oldtime).count());
                    PVR_DB("[StreamReceiver th] ------------------- Stream Receiving @ FPS: " +
                           to_string(fpsStreamRecver) + " De-coding @ FPS : " +
                           to_string(fpsStreamDecoder) + " Rendering @ FPS : " +
                           to_string(fpsRenderer));
                    oldtime = Clk::now();

                    updateJavaTextViewFPS(fpsStreamRecver, fpsStreamDecoder, fpsRenderer,
                                          fpsBuf[0], fpsBuf[1], fpsBuf[2], fpsBuf[3], fpsBuf[4],
                                          ctdBuf[0], ctdBuf[1],
                                          networkDelay, (int) ((duration_cast<microseconds>(
                                    system_clock::now().time_since_epoch()).count() - *timestamp) /
                                                               1000));
                }
                delMtx.lock();
                videoSvc = nullptr;
                delMtx.unlock();
            }
            catch (exception &e) {
                PVR_DB_I("[PVRStartReceiveStreams th] caught Exception: " + to_string(e.what()));
            }
        });
    }
    catch(exception e) {
        PVR_DB_I("PVRSockets_PVRStartReceiveStreams:: Caught Exception: " + string(e.what()));
    }
}

void PVRStopStreams() {
    try {
        // talker sends disconnects at segue
        delMtx.lock();
        if (videoSvc)
            videoSvc->stop(); //todo: use mutex
        delMtx.unlock();

        if (strThr) {
            strThr->join();
            delete strThr;
            strThr = nullptr;
        }
    }
    catch(exception e) {
        PVR_DB_I("PVRSockets_PVRStopStreams:: Caught Exception: " + string(e.what()));
    }
}
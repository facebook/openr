#pragma once

#include <iostream>
#include <memory>
#include <string>
#include <csignal>
#include <sys/socket.h>

#include <grpc++/grpc++.h>
#include <openr/servicelayer/sl_global.grpc.pb.h>
#include <openr/servicelayer/sl_global.pb.h>
#include <openr/servicelayer/sl_common_types.pb.h>
#include <openr/servicelayer/sl_version.pb.h>
#include <openr/servicelayer/sl_route_common.pb.h>
#include <openr/servicelayer/sl_route_ipv4.grpc.pb.h>
#include <openr/servicelayer/sl_route_ipv6.grpc.pb.h>
#include <openr/servicelayer/sl_route_ipv4.pb.h>
#include <openr/servicelayer/sl_route_ipv6.pb.h>

#include <thread>
#include <typeinfo>
#include <condition_variable>

namespace openr {


extern std::mutex m_mutex;
extern std::condition_variable m_condVar;
extern bool m_InitSuccess;

class RShuttle {
public:
    explicit RShuttle(std::shared_ptr<grpc::Channel> Channel);

    std::shared_ptr<grpc::Channel> channel;
    service_layer::SLRegOp routeOp;

};


class SLVrf {
public:
    explicit SLVrf(std::shared_ptr<grpc::Channel> Channel);

    std::shared_ptr<grpc::Channel> channel;
    service_layer::SLRegOp vrf_op;
    service_layer::SLVrfRegMsg vrf_msg;
    service_layer::SLVrfRegMsgRsp vrf_msg_resp;

    void vrfRegMsgAdd(std::string vrfName,
                      unsigned int adminDistance,
                      unsigned int vrfPurgeIntervalSeconds);

    void registerVrf(unsigned int addrFamily);

    void unregisterVrf(unsigned int addrFamily);

    void vrfOpv4();

    void vrfOpv6();

};


class AsyncNotifChannel {
public:
    explicit AsyncNotifChannel(std::shared_ptr<grpc::Channel> channel);

    void SendInitMsg(const service_layer::SLInitMsg init_msg);

    void AsyncCompleteRpc();

private:
    // Out of the passed in Channel comes the stub, stored here, our view of the
    // server's exposed services.
    std::unique_ptr<service_layer::SLGlobal::Stub> stub_;

    // The producer-consumer queue we use to communicate asynchronously with the
    // gRPC runtime.
    grpc::CompletionQueue cq_;

    // Used as an indicator to exit completion queue thread upon queue shutdown.
    bool tearDown = false;


    class ResponseHandler {
    public:
        virtual void HandleResponse(bool eventStatus, grpc::CompletionQueue* pcq_)=0;
        virtual ~ResponseHandler();
    };


    class AsyncClientCall: public ResponseHandler {
    private:
        enum CallStatus {CREATE, PROCESS, FINISH};
        CallStatus callStatus_;
    public:
        AsyncClientCall();
        // Container for the data we expect from the server.
        service_layer::SLGlobalNotif notif;
        // Context for the client. It could be used to convey extra information to
        // the server and/or tweak certain RPC behaviors.
        grpc::ClientContext context;

        // Storage for the status of the RPC upon completion.
        grpc::Status status;

        //std::unique_ptr<ClientAsyncResponseReader<HelloReply>> response_reader;
        std::unique_ptr< ::grpc::ClientAsyncReaderInterface< ::service_layer::SLGlobalNotif>> response_reader;

        void HandleResponse(bool responseStatus, grpc::CompletionQueue* pcq_) override;
        
    };

};

}

#pragma once

#include <stdint.h>
#include <thread>
#include <condition_variable>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <glog/logging.h>
#include <arpa/inet.h>

#include <grpc++/grpc++.h>
/*#include <iosxrsl/sl_global.grpc.pb.h>
#include <iosxrsl/sl_global.pb.h>
#include <iosxrsl/sl_common_types.pb.h>
#include <iosxrsl/sl_version.pb.h>
*/

#include <xrtelemetry/telemetry.pb.h>
#include <xrtelemetry/telemetry.grpc.pb.h>
#include <xrtelemetry/mdt_grpc_dialin/mdt_grpc_dialin.grpc.pb.h>
#include <xrtelemetry/mdt_grpc_dialin/mdt_grpc_dialin.pb.h>

namespace openr {

extern std::mutex init_mutex;
extern std::condition_variable init_condVar;
extern bool init_success;

class AsyncNotifChannel {
public:
    explicit AsyncNotifChannel(std::shared_ptr<grpc::Channel> channel);

    void SendInitMsg(const IOSXRExtensibleManagabilityService::CreateSubsArgs args);
    void AsyncCompleteRpc();

    void Shutdown();
    void Cleanup();

    std::mutex channel_mutex;
    std::condition_variable channel_condVar;
    bool channel_closed = false;

private:
    // Out of the passed in Channel comes the stub, stored here, our view of the
    // server's exposed services.
    std::unique_ptr<IOSXRExtensibleManagabilityService::gRPCConfigOper::Stub> stub_;

    // The producer-consumer queue we use to communicate asynchronously with the
    // gRPC runtime.
    grpc::CompletionQueue cq_;


    // Used as an indicator to exit completion queue thread upon queue shutdown.
    bool tear_down = false;

    class AsyncClientCall {
    private:
        enum CallStatus {CREATE, PROCESS, FINISH};
        CallStatus callStatus_;
    public:
        AsyncClientCall();
        // Container for the data we expect from the server.
        IOSXRExtensibleManagabilityService::CreateSubsReply createSubsReply;
        // Context for the client. It could be used to convey extra information to
        // the server and/or tweak certain RPC behaviors.
        grpc::ClientContext context;

        // Storage for the status of the RPC upon completion.
        grpc::Status status;

        std::unique_ptr< ::grpc::ClientAsyncReaderInterface< ::IOSXRExtensibleManagabilityService::CreateSubsReply>> response_reader;

        void HandleResponse(bool responseStatus, grpc::CompletionQueue* pcq_);      

    } call;

};

}

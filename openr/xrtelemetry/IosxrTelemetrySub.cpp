#include "IosxrTelemetrySub.h" 
#include <google/protobuf/text_format.h>
#include <google/protobuf/util/json_util.h>

using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::CompletionQueue;
using grpc::Status;


namespace openr {

std::mutex init_mutex;
std::condition_variable init_condVar;
bool init_success;


AsyncNotifChannel::AsyncNotifChannel(std::shared_ptr<grpc::Channel> channel)
        : stub_(IOSXRExtensibleManagabilityService::gRPCConfigOper::NewStub(channel)) {}


// Assembles the client's payload and sends it to the server.

void 
AsyncNotifChannel::SendInitMsg(const IOSXRExtensibleManagabilityService::CreateSubsArgs args)
{
    std::string s;

    if (google::protobuf::TextFormat::PrintToString(args, &s)) {
        VLOG(2) << "###########################" ;
        VLOG(2) << "Transmitted message: IOSXR-SL INIT " << s;
        VLOG(2) << "###########################" ;
    } else {
        VLOG(2) << "###########################" ;
        VLOG(2) << "Message not valid (partial content: "
                  << args.ShortDebugString() << ")";
        VLOG(2) << "###########################" ;
    }

    // Typically when using the asynchronous API, we hold on to the 
    //"call" instance in order to get updates on the ongoing RPC.
    // In our case it isn't really necessary, since we operate within the
    // context of the same class, but anyway, we pass it in as the tag

    call.context.AddMetadata("username", "root");
    call.context.AddMetadata("password", "lab");

    call.response_reader = stub_->AsyncCreateSubs(&call.context, args, &cq_, (void *)&call);
}

void 
AsyncNotifChannel::Shutdown() 
{
    tear_down = true;

    std::unique_lock<std::mutex> channel_lock(channel_mutex);

    while(!channel_closed) {
        channel_condVar.wait(channel_lock);
    }
}


void 
AsyncNotifChannel::Cleanup() 
{
    VLOG(1) << "Asynchronous client shutdown requested"
            << "Let's clean up!";

    // Finish the Async session
    call.HandleResponse(false, &cq_);

    // Shutdown the completion queue
    call.HandleResponse(false, &cq_);

    VLOG(1) << "Notifying channel close";
    channel_closed = true;
    // Notify the condition variable;
    channel_condVar.notify_one();
}


// Loop while listening for completed responses.
// Prints out the response from the server.
void 
AsyncNotifChannel::AsyncCompleteRpc() 
{
    void* got_tag;
    bool ok = false;
    // Storage for the status of the RPC upon completion.
    grpc::Status status;

    // Lock the mutex before notifying using the conditional variable
    std::lock_guard<std::mutex> guard(channel_mutex);


    unsigned int timeout = 5;

    // Set timeout for API
    std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::now() + std::chrono::seconds(timeout);

    while (!tear_down) {
        auto nextStatus = cq_.AsyncNext(&got_tag, &ok, deadline);

        switch(nextStatus) {
        case grpc::CompletionQueue::GOT_EVENT:
             // Verify that the request was completed successfully. Note that "ok"
             // corresponds solely to the request for updates introduced by Finish().
             call.HandleResponse(ok, &cq_);
             break;
        case grpc::CompletionQueue::SHUTDOWN:
             VLOG(1) << "Shutdown event received for completion queue";
             channel_closed = true;
             // Notify the condition variable;
             channel_condVar.notify_one();
             tear_down = true;
             break;
        case grpc::CompletionQueue::TIMEOUT:
             continue;
             break;
        }
    }

    if(!channel_closed) {
        Cleanup();
    }
}


AsyncNotifChannel::AsyncClientCall::AsyncClientCall(): callStatus_(CREATE) {}

void 
AsyncNotifChannel::AsyncClientCall::HandleResponse(bool responseStatus, 
                                                   grpc::CompletionQueue* pcq_)
{
    //The First completion queue entry indicates session creation and shouldn't be processed - Check?
    switch (callStatus_) {
    case CREATE:
        if (responseStatus) {
            response_reader->Read(&createSubsReply, (void*)this);
           // LOG(INFO) << createSubsReply.errors();
           // LOG(INFO) << createSubsReply.data();

            std::string json_string;
            google::protobuf::util::JsonPrintOptions options;
            options.add_whitespace = true;
            options.always_print_primitive_fields = true;
            options.preserve_proto_field_names = true;
            google::protobuf::util::MessageToJsonString(createSubsReply, &json_string, options);

            // Print json_string.
            //std::cout << json_string << std::endl;
            json_string.clear();

            /*auto telemetry_data = telemetry::Telemetry();           
            telemetry_data.CopyFrom(createSubsReply);

            google::protobuf::util::MessageToJsonString(telemetry_data, &json_string, options);

            // Print json_string.
            std::cout << json_string << std::endl;*/
            json_string.clear();

            callStatus_ = PROCESS;
        } else {
            response_reader->Finish(&status, (void*)this);
            callStatus_ = FINISH;
        }
        break;
    case PROCESS:
        if (responseStatus) {
            response_reader->Read(&createSubsReply, (void *)this);
           // LOG(INFO) << createSubsReply.errors();
           // LOG(INFO) << createSubsReply.data();
            std::string json_string;
            google::protobuf::util::JsonPrintOptions options;
            options.add_whitespace = true;
            options.always_print_primitive_fields = true;
            options.preserve_proto_field_names = true;
            google::protobuf::util::MessageToJsonString(createSubsReply, &json_string, options);


            // Print json_string.
            //std::cout << json_string << std::endl;
            json_string.clear(); 
            
            auto telemetry_data = telemetry::Telemetry();    
            telemetry_data.ParseFromString(createSubsReply.data());

            google::protobuf::util::MessageToJsonString(telemetry_data, &json_string, options);

            // Print json_string.
            std::cout << json_string << std::endl;
            json_string.clear();

            auto telemetry_gpb_table = telemetry_data.data_gpb();

            for (int row_index=0; row_index < telemetry_gpb_table.row_size(); row_index++) {
                auto telemetry_gpb_row = telemetry_gpb_table.row(row_index);
                google::protobuf::util::MessageToJsonString(telemetry_gpb_row, &json_string, options);
                std::cout << "\n\n\n\n############################\n\n Start of row\n\n\n";
                std::cout << json_string << std::endl;
                json_string.clear();
                std::cout << "\n\n\n End of row\n\n############################\n\n\n\n";
            } 
        }
        break;
    case FINISH:
        if (status.ok()) {
            VLOG(1) << "Server Response Completed: "  
                    << this << " CallData: " 
                    << this;
        }
        else {
            LOG(ERROR) << "RPC failed";
        }
        VLOG(1) << "Shutting down the completion queue";
        pcq_->Shutdown();
    }
}

}

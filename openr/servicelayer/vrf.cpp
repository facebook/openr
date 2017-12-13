#include <openr/servicelayer/vrf.h>

using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::CompletionQueue;
using grpc::Status;
using service_layer::SLInitMsg;
using service_layer::SLVersion;
using service_layer::SLGlobal;

namespace openr {

std::mutex m_mutex;
std::condition_variable m_condVar;
bool m_InitSuccess;

SLVrf::SLVrf(std::shared_ptr<grpc::Channel> Channel)
    : channel(Channel) {}



void SLVrf::vrfRegMsgAdd(std::string vrfName,
                         unsigned int adminDistance,
                         unsigned int vrfPurgeIntervalSeconds) {

    // Get a pointer to a new vrf_reg entry in vrf_msg
    service_layer::SLVrfReg* vrf_reg = vrf_msg.add_vrfregmsgs();

    // Populate the new vrf_reg entry
    vrf_reg->set_vrfname(vrfName);
    vrf_reg->set_admindistance(adminDistance);
    vrf_reg->set_vrfpurgeintervalseconds(vrfPurgeIntervalSeconds);
}


void SLVrf::registerVrf(unsigned int addrFamily) {

    // Send an RPC for VRF registrations

    switch(addrFamily) {
    case AF_INET:
        std::cout << "IPv4 VRF Operation" << std::endl;

        vrf_op = service_layer::SL_REGOP_REGISTER;
        vrfOpv4();

        // RPC EOF to cleanup any previous stale routes
        vrf_op = service_layer::SL_REGOP_EOF;
        vrfOpv4();

        break;

    case AF_INET6:
        std::cout << "IPv6 VRF Operation" << std::endl;

        vrf_op = service_layer::SL_REGOP_REGISTER;
        vrfOpv6();

        // RPC EOF to cleanup any previous stale routes
        vrf_op = service_layer::SL_REGOP_EOF;
        vrfOpv6();

        break;            

    default:
        std::cout << "Invalid Address family, skipping.." << std::endl;
        break;
    }

}

void SLVrf::unregisterVrf(unsigned int addrFamily) {

    //  When done with the VRFs, RPC Delete Registration

    switch(addrFamily) {
    case AF_INET:
        std::cout << "IPv6 VRF Operation" << std::endl;

        vrf_op = service_layer::SL_REGOP_UNREGISTER;
        vrfOpv6();
            
        break;

    case AF_INET6:
        std::cout << "IPv6 VRF Operation" << std::endl;
        
        vrf_op = service_layer::SL_REGOP_UNREGISTER;
        vrfOpv6();
        
        break;

    default:
        std::cout << "Invalid Address family, skipping.." << std::endl;
        break;
    }
}

void SLVrf::vrfOpv4() {
    // Set up the RouteV4Oper Stub
    auto stub_ = service_layer::SLRoutev4Oper::NewStub(channel);

    // Context for the client. It could be used to convey extra information to
    // the server and/or tweak certain RPC behaviors.
    grpc::ClientContext context;

    // Storage for the status of the RPC upon completion.
    grpc::Status status;        

    unsigned int timeout = 10;
        // Set timeout for API
    std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::now() + std::chrono::seconds(timeout);

    context.set_deadline(deadline);

    // Set up vrfRegMsg Operation

    vrf_msg.set_oper(vrf_op);

    //Issue the RPC         

    status = stub_->SLRoutev4VrfRegOp(&context, vrf_msg, &vrf_msg_resp);

    if (status.ok()) {
        std::cout << "RPC call was successful, checking response..." << std::endl;


        if (vrf_msg_resp.statussummary().status() ==
               service_layer::SLErrorStatus_SLErrno_SL_SUCCESS) {

            std::cout << "IPv4 Vrf Operation:"<< vrf_op << " Successful" << std::endl;
        } else {
            std::cout << "Error code for VRF Operation:" << vrf_op << " is 0x" << std::hex << vrf_msg_resp.statussummary().status() << std::endl;

            // Print Partial failures within the batch if applicable
            if (vrf_msg_resp.statussummary().status() ==
                    service_layer::SLErrorStatus_SLErrno_SL_SOME_ERR) {
                for (int result = 0; result < vrf_msg_resp.results_size(); result++) {
                      auto slerr_status = static_cast<int>(vrf_msg_resp.results(result).errstatus().status());
                      std::cout << "Error code for vrf " << vrf_msg_resp.results(result).vrfname() << " is 0x" << std::hex << slerr_status << std::endl;
                }
            } 
        }
    } else {
        std::cout << "RPC failed, error code is " << status.error_code() << std::endl;
    }
}
                    
void SLVrf::vrfOpv6() {

    // Set up the RouteV4Oper Stub
    auto stub_ = service_layer::SLRoutev6Oper::NewStub(channel);


    // Context for the client. It could be used to convey extra information to
    // the server and/or tweak certain RPC behaviors.
    grpc::ClientContext context;
    
    // Storage for the status of the RPC upon completion.
    grpc::Status status;
    
    unsigned int timeout = 10;
    // Set timeout for API
    std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::now() + std::chrono::seconds(timeout);
     
    context.set_deadline(deadline);

    // Set up vrfRegMsg Operation

    vrf_msg.set_oper(vrf_op);

    //Issue the RPC         

    status = stub_->SLRoutev6VrfRegOp(&context, vrf_msg, &vrf_msg_resp);

    if (status.ok()) {
        std::cout << "RPC call was successful, checking response...";
        if (vrf_msg_resp.statussummary().status() ==
               service_layer::SLErrorStatus_SLErrno_SL_SUCCESS) {
            std::cout << "IPv6 Vrf Operation: "<< vrf_op << " successful" << std::endl;
        } else {
            std::cout << "Error code for VRF Operation:" << vrf_op << " is 0x" << std::hex << vrf_msg_resp.statussummary().status() << std::endl;

            // Print Partial failures within the batch if applicable
            if (vrf_msg_resp.statussummary().status() ==
                    service_layer::SLErrorStatus_SLErrno_SL_SOME_ERR) {
                for (int result = 0; result < vrf_msg_resp.results_size(); result++) {
                    auto slerr_status = static_cast<int>(vrf_msg_resp.results(result).errstatus().status());
                    std::cout << "Error code for vrf " << vrf_msg_resp.results(result).vrfname() << " is 0x" << std::hex << slerr_status << std::endl;
                }
            }
        }
    } else {
        std::cout << "RPC failed, error code is " << status.error_code() << std::endl;
    }

}
 
AsyncNotifChannel::AsyncNotifChannel(std::shared_ptr<grpc::Channel> channel)
        : stub_(service_layer::SLGlobal::NewStub(channel)) {}


// Assembles the client's payload and sends it to the server.

void AsyncNotifChannel::SendInitMsg(const service_layer::SLInitMsg init_msg) {
    // Call object to store rpc data
    AsyncClientCall* call = new AsyncClientCall;

    // Because we are using the asynchronous API, we need to
    // hold on to the "call" instance in order to get updates on the ongoing RPC.

    //std::unique_ptr< ::grpc::ClientAsyncReaderInterface< ::service_layer::SLGlobalNotif>> response_reader;

    call->response_reader = stub_->AsyncSLGlobalInitNotif(&call->context, init_msg, &cq_, (void *)call);
}

// Loop while listening for completed responses.
// Prints out the response from the server.
void AsyncNotifChannel::AsyncCompleteRpc() {
    void* got_tag;
    bool ok = false;

    unsigned int timeout = 365*24*60*60;
    // Set timeout for API
    std::chrono::system_clock::time_point deadline =
        std::chrono::system_clock::now() + std::chrono::seconds(timeout);

 
    while (!tearDown) {
        auto nextStatus = cq_.AsyncNext(&got_tag, &ok, deadline);
        // The tag is the memory location of the call object
        ResponseHandler* responseHandler = static_cast<ResponseHandler*>(got_tag);

        switch(nextStatus) {
        case grpc::CompletionQueue::GOT_EVENT:
             // Verify that the request was completed successfully. Note that "ok"
             // corresponds solely to the request for updates introduced by Finish().
             responseHandler->HandleResponse(ok, &cq_);
             break;
        case grpc::CompletionQueue::SHUTDOWN:
             std::cout << "Shutdown event received for completion queue, shutdown the thread" << std::endl;
             delete responseHandler;
             tearDown = true;
             break;
        case grpc::CompletionQueue::TIMEOUT:
             break;
        }
    }
}


AsyncNotifChannel::ResponseHandler::~ResponseHandler()
{
    std::cout << "Cleaning up the ResponseHandler object" << std::endl;
}

// struct for keeping state and data information
AsyncNotifChannel::AsyncClientCall::AsyncClientCall(): callStatus_(CREATE) {}

void AsyncNotifChannel::AsyncClientCall::HandleResponse(bool responseStatus, grpc::CompletionQueue* pcq_) {
    //The First completion queue entry indicates session creation and shouldn't be processed - Check?
    switch (callStatus_) {
    case CREATE:
        if (responseStatus) {
            response_reader->Read(&notif, (void*)this);
            callStatus_ = PROCESS;
        } else {
            response_reader->Finish(&status, (void*)this);
            callStatus_ = FINISH;
        }
        break;
    case PROCESS:
        if (responseStatus) {
            response_reader->Read(&notif, (void *)this);
            auto slerrstatus = static_cast<int>(notif.errstatus().status());
            auto eventtype = static_cast<int>(notif.eventtype());

            if( eventtype == static_cast<int>(service_layer::SL_GLOBAL_EVENT_TYPE_VERSION) ) {
                if((slerrstatus == 
                       service_layer::SLErrorStatus_SLErrno_SL_SUCCESS) ||
                   (slerrstatus == 
                       service_layer::SLErrorStatus_SLErrno_SL_INIT_STATE_READY) ||
                   (slerrstatus == 
                       service_layer::SLErrorStatus_SLErrno_SL_INIT_STATE_CLEAR)) {
                    std::cout << "Server returned " ; 
                    std::cout << "Successfully Initialized, connection Established!" << std::endl;
                            
                    // Lock the mutex before notifying using the conditional variable
                    std::lock_guard<std::mutex> guard(m_mutex);

                    // Set the initsuccess flag to indicate successful initialization
                    m_InitSuccess = true;
       
                    // Notify the condition variable;
                    m_condVar.notify_one();

                } else {
                    std::cout << "client init error code " << slerrstatus << std::endl;
                }
            } else if (eventtype == static_cast<int>(service_layer::SL_GLOBAL_EVENT_TYPE_HEARTBEAT)) {
                std::cout << "Received Heartbeat" << std::endl; 
            } else if (eventtype == static_cast<int>(service_layer::SL_GLOBAL_EVENT_TYPE_ERROR)) {
                if (slerrstatus == service_layer::SLErrorStatus_SLErrno_SL_NOTIF_TERM) {
                    std::cout << "Received notice to terminate. Client Takeover?" << std::endl;
                } else {
                    std::cout << "Error Not Handled " << slerrstatus << std::endl;
                } 
            } else {
                std::cout << "client init unrecognized response " << eventtype << std::endl;
            }
        } else {
            response_reader->Finish(&status, (void*)this);
            callStatus_ = FINISH;
        }
        break;
    case FINISH:
        if (status.ok()) {
            std::cout << "Server Response Completed: " << this << " CallData: " << this << std::endl;
        }
        else {
            std::cout << "RPC failed" << std::endl;
        }
        std::cout << "Shutting down the completion queue" << std::endl;
        pcq_->Shutdown();
    }
} 


/*int main(int argc, char** argv) {
    AsyncNotifChannel asynchandler(grpc::CreateChannel(
                              "14.1.1.20:57777", grpc::InsecureChannelCredentials()));

    // Acquire the lock
    std::unique_lock<std::mutex> mlock(m_mutex);

    // Spawn reader thread that maintains our Notification Channel
    std::thread thread_ = std::thread(&AsyncNotifChannel::AsyncCompleteRpc, &asynchandler);


    service_layer::SLInitMsg init_msg;
    init_msg.set_majorver(service_layer::SL_MAJOR_VERSION);
    init_msg.set_minorver(service_layer::SL_MINOR_VERSION);
    init_msg.set_subver(service_layer::SL_SUB_VERSION);


    asynchandler.SendInitMsg(init_msg);  

    // Wait on the mutex lock
    while (!m_InitSuccess) {
        m_condVar.wait(mlock);
    }

    SLVrf vrfhandler(grpc::CreateChannel(
                              "14.1.1.20:57777", grpc::InsecureChannelCredentials()));

    // Create a new SLVrfRegMsg batch
    vrfhandler.vrfRegMsgAdd("test123", 10, 500);
    vrfhandler.vrfRegMsgAdd("test1", 12, 500);
    vrfhandler.vrfRegMsgAdd("test2", 13, 500);
    vrfhandler.vrfRegMsgAdd("test3", 14, 500);
    vrfhandler.vrfRegMsgAdd("test4", 15, 500);
    vrfhandler.vrfRegMsgAdd("test5", 16, 500);
    vrfhandler.vrfRegMsgAdd("test6", 17, 500);

    // Register the SLVrfRegMsg batch for v4 and v6
    vrfhandler.registerVrf(AF_INET);
    vrfhandler.registerVrf(AF_INET6);


    // Clean up the SLVrfRegMsg batch to start again
    vrfhandler.vrf_msg.clear_vrfregmsgs();

    // Creating a fresh SLVrfRegMsg batch
    vrfhandler.vrfRegMsgAdd("test", 10, 500);
    vrfhandler.vrfRegMsgAdd("test1", 12, 500);
    vrfhandler.vrfRegMsgAdd("test2", 13, 500);

    // Unregister the SLVrfRegMsg batch this time
    vrfhandler.unregisterVrf(AF_INET);
    vrfhandler.unregisterVrf(AF_INET6);


    std::cout << "Press control-c to quit" << std::endl << std::endl;
    thread_.join();

    return 0;
}*/
}

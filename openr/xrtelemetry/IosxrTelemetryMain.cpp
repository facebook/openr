#include "IosxrTelemetrySub.h"
#include <csignal>

using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::CompletionQueue;
using grpc::Status;
using namespace openr;

std::string 
getEnvVar(std::string const & key)
{
    char * val = std::getenv( key.c_str() );
    return val == NULL ? std::string("") : std::string(val);
}


TelemetryStream* asynchandler_signum;

bool sighandle_initiated = false;

void 
signalHandler(int signum)
{

   if (!sighandle_initiated) {
       sighandle_initiated = true;
       VLOG(1) << "Interrupt signal (" << signum << ") received.";

       // Shutdown the Async Notification Channel  
       asynchandler_signum->Shutdown();

       //terminate program  
       //exit(signum);  
    } 
}

int main(int argc, char** argv) {
   
    auto server_ip = getEnvVar("SERVER_IP");
    auto server_port = getEnvVar("SERVER_PORT");

    if (server_ip == "" || server_port == "") {
        if (server_ip == "") {
            LOG(ERROR) << "SERVER_IP environment variable not set";
        }
        if (server_port == "") {
            LOG(ERROR) << "SERVER_PORT environment variable not set";
        }
        return 1;

    }


    std::string grpc_server = server_ip + ":" + server_port;
    auto channel = grpc::CreateChannel(
                             grpc_server, grpc::InsecureChannelCredentials());

 
    LOG(INFO) << "Connecting IOS-XR to gRPC server at " << grpc_server;

    TelemetryStream asynchandler(channel);

    // Spawn reader thread that maintains our Notification Channel
    std::thread thread_ = std::thread(&TelemetryStream::AsyncCompleteRpc, &asynchandler);



    asynchandler.SetCredentials("root", "lab");

    asynchandler.AddSubscription(99,
                                 IOSXR_TELEMETRY_DIALIN_GPB,
                                 "LLDP");

    asynchandler.AddSubscription(99,
                                 IOSXR_TELEMETRY_DIALIN_GPB,
                                "INTERFACESSUB");

    asynchandler.SubscribeAll();

    asynchandler_signum = &asynchandler;

    signal(SIGINT, signalHandler);  
    LOG(INFO) << "Press control-c to quit";
    thread_.join();

    return 0;
}

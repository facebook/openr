FROM ubuntu:16.04

RUN apt-get update && apt-get install -y git iproute2 vim && git clone https://github.com/Cisco-Service-Layer/service-layer-objmodel ~/service-layer-objmodel && \
           cd ~/service-layer-objmodel/grpc/cpp && \
           ./build_libiosxrsl.sh -g 1.9.1 -p 3.5.0 &&  \
           cd ~/ && rm -r ~/service-layer-objmodel 

RUN git clone https://wwwin-github.cisco.com/arhashem/openr-xr.git -b openr-merge-final /root/openr-dependencies

RUN cd /root/openr-dependencies/build && ./build_openr_dependencies.sh && cd ~/ && rm -r /root/openr-dependencies

RUN apt-get install -y net-tools && git clone https://wwwin-github.cisco.com/arhashem/openr-xr -b openr-merge-final /root/openr-xr/ && cd /root/openr-xr/build && ./build_openr.sh && ./remake_glog.sh && cd /root && rm -r /root/openr-xr && rm -r /usr/local/sbin/tests

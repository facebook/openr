FROM ubuntu:16.04 

ENV http_proxy 'http://proxy-wsa.esl.cisco.com:80'
ENV https_proxy 'http://proxy-wsa.esl.cisco.com:80'

RUN apt-get update && apt-get install -y autoconf automake libtool curl make g++ unzip git python-pip python-dev && git clone https://github.com/google/protobuf.git ~/protobuf && \
            cd ~/protobuf && \
            git checkout 2761122b810fe8861004ae785cc3ab39f384d342 && \
            ./autogen.sh && \
            ./configure && \
            make && \
            make install &&\
            ldconfig && make clean && cd ~/ && rm -r ~/protobuf 

RUN git clone https://github.com/grpc/grpc.git ~/grpc && cd ~/grpc && \
            git checkout 80893242c1ee929d19e6bec5dc19a1515cd8dd81 && \
            git submodule update --init && \
            make && \
            make install && make clean && cd ~/ && rm -r ~/grpc

RUN apt-get install -y pkg-config && git clone https://wwwin-github.cisco.com/akshshar/service-layer-objmodel ~/service-layer-objmodel && \
           cd ~/service-layer-objmodel/grpc/cpp && \
           ./build_libiosxrsl.sh &&  \
           cd ~/ && rm -r ~/service-layer-objmodel

RUN git clone https://github.com/akshshar/openr.git /root/openr && cd /root/openr/ && git checkout openr20171212 && cd /root/openr/build && ./build_openr_dependencies.sh

RUN cd /root/openr/build && ./build_openr.sh && ./remake_glog.sh && cd /root/ && rm -r /root/openr

COPY run_openr.sh /usr/sbin/run_openr.sh

CMD /usr/sbin/run_openr.sh >/var/log/openr.log 2>&1


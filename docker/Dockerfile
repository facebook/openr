FROM ubuntu:16.04

RUN apt-get update && apt-get install -y git 

RUN git clone https://github.com/akshshar/openr.git /root/openr

RUN cd /root/openr/build/ && ./build_openr.sh && ./build_openr.sh && cd /root && rm -r /root/openr

COPY run_openr.sh /usr/sbin/run_openr.sh

CMD /usr/sbin/run_openr.sh 


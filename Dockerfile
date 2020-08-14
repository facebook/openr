FROM ubuntu:20.04

# Install tools needed for development
RUN apt update && \
    apt upgrade --yes && \
    apt install --yes build-essential git libssl-dev m4 python3-pip

# Copy needed source
RUN mkdir /src
ADD CMakeLists.txt FBGenCMakeBuildInfo.cmake ThriftLibrary.cmake /src/
COPY build /src/build
COPY openr /src/openr
COPY example_openr.conf /etc/openr.conf

# Build OpenR + Dependencies via cmake
RUN cd /src && build/build_openr.sh && chmod 644 /etc/openr.conf
RUN mkdir /opt/bin && cp /src/build/docker_openr_helper.sh /opt/bin

# Install `breeze` OpenR CLI
RUN pip3 --no-cache-dir install --upgrade pip setuptools wheel
RUN PATH="${PATH}:/opt/facebook/fbthrift/bin" ; \
    cd /src/openr/py/ && \
    python3 setup.py install

# Cleanup all we can to keep container as lean as possible
RUN apt remove --yes build-essential git libssl-dev m4 && \
    apt autoremove --yes && \
    rm -rf /src /tmp/* /var/lib/apt/lists/*

CMD ["/opt/bin/docker_openr_helper.sh"]
# Expose OpenR Thrift port - Recommended to run OpenR on host network ...
EXPOSE 2018/tcp

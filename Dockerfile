FROM ubuntu:20.04


# Install tools needed for development
RUN apt update && \
    apt upgrade --yes && \
    apt install --yes build-essential cython3 git libssl-dev m4 python3-pip g++-10

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
RUN git clone https://github.com/cython/cython
RUN cd /src && build/build_breeze.sh
RUN cp -r /src/build/lib.linux-x86_64-3.8 /breeze-build

# Cleanup all we can to keep container as lean as possible
# TODO: We can use Dockerfile stages instead
RUN apt remove --yes build-essential git libssl-dev m4 g++-10 && \
    apt autoremove --yes && \
    rm -rf /src /tmp/* /var/lib/apt/lists/*

CMD ["/opt/bin/docker_openr_helper.sh"]
# Expose OpenR Thrift port
EXPOSE 2018/tcp

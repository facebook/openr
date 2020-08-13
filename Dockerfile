FROM ubuntu:20.04

# Install tools needed for development
RUN apt update
RUN apt upgrade --yes
RUN apt install --yes build-essential git libssl-dev m4 python3-pip

# Copy needed source
RUN mkdir /src
ADD CMakeLists.txt FBGenCMakeBuildInfo.cmake ThriftLibrary.cmake /src/
COPY build /src/build
COPY openr /src/openr

# Build OpenR + Dependencies via cmake
RUN cd /src && build/build_openr.sh
RUN mkdir /opt/bin && cp /src/build/docker_openr_helper.sh /opt/bin

# Install `breeze` OpenR CLI
RUN pip3 --no-cache-dir install --upgrade pip setuptools wheel
RUN PATH="${PATH}:/opt/facebook/fbthrift/bin" ; cd /src/openr/py/ && python3 setup.py install

# Cleanup all we can to keep container as lean as possible
RUN apt remove --yes build-essential git libssl-dev m4
RUN apt autoremove --yes
RUN rm -r /src /tmp/*

# TODO: Fix and make a sane default with a default config
# Also have ability for a bind mounted config directory + state file
CMD ["/opt/bin/docker_openr_helper.sh"]

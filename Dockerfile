FROM ubuntu:18.04

# Install tools needed for development
RUN apt update
RUN apt upgrade --yes
RUN apt install --yes build-essential git m4 python3-pip

# Copy needed source
RUN mkdir /src
ADD CMakeLists.txt FBGenCMakeBuildInfo.cmake ThriftLibrary.cmake /src/
COPY build /src/build
COPY openr /src/openr

# Build OpenR + Dependencies via cmake
RUN cd /src && build/build_openr.sh
RUN cd /src/build && make install

# Install `breeze` OpenR CLI
RUN pip --no-cache-dir install --upgrade pip setuptools wheel
RUN cd /src/openr/py/ && python3 setup.py install

# Cleanup all we can to keep container as lean as possible
RUN apt remove --yes build-essential
RUN rm -r /src

# TODO: Fix and make a sane default with a default config
# Also have ability for a bind mounted config directory + state file
CMD sleep 3600

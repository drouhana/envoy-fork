FROM ubuntu:20.04

# update package manager
RUN apt-get update -y \
    && apt-get install -y autoconf automake cmake curl libtool make ninja-build patch python3-pip unzip virtualenv

# install bazelisk as bazel
RUN wget -O /usr/local/bin/bazel https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-$([ $(uname -m) = "aarch64" ] && echo "arm64" || echo "amd64") \
    && chmod +x /usr/local/bin/bazel

# copy files
COPY . /usr/local/envoy 

# build
WORKDIR /usr/local/evoy
RUN bazel build -c opt envoy

# note: user.bazelrc currently has the following build arguments in it:
#   build --config=libc++ --define quiche=disabled --define wasm=disabled --define deprecated_features=disabled --//bazel:http3=false --//source/extensions/quic/crypto_stream:enabled=false --//source/extensions/quic/proof_source:enabled=false

WORKDIR /usr/local/evoy
CMD ["/usr/local/envoy/bazel-out/k8-opt/bin/source/exe/envoy-static", "-c /usr/local/envoy/configs/envoy-demo.yaml" ]
# syntax=docker/dockerfile1.2

ARG UBUNTU_VERSION=20.04
ARG UBUNTU_IMAGE="ubuntu:${UBUNTU_VERSION}"

# Build container based on Ubuntu 18.04
FROM ${UBUNTU_IMAGE} AS base
RUN echo 'Binary::apt::APT::Keep-Downloaded-Packages "true";' > /etc/apt/apt.conf.d/keep-cache
ARG ATP_MIIOR
RUN echo 'Acquire::http::proxy "http://child-prc.intel.com:913/";' > /etc/apt/apt.conf.d/proxy.conf
RUN echo 'Acquire::https::proxy "http://child-prc.intel.com:913/";' >> /etc/apt/apt.conf.d/proxy.conf
RUN echo 'Acquire::ftp::proxy "ftp://child-prc.intel.com:913/";' >> /etc/apt/apt.conf.d/proxy.conf 
RUN echo 'Acquire::socks::proxy "socks://child-prc.intel.com:913/";' >> /etc/apt/apt.conf.d/proxy.conf
ENV http_proxy=http://child-prc.intel.com:913
ENV https_proxy=http://child-prc.intel.com:913
ENV DEBIAN_FRONTEND=noninteractive

FROM base AS ACRN

RUN useradd -ms /bin/bash  acrn
WORKDIR /Workspace
# Install dependencies.
RUN apt-get update \
    && apt-get install -y gcc make vim git \
                          gnu-efi \
                          libssl-dev \
                          libpciaccess-dev \
                          uuid-dev \
                          libsystemd-dev \
                          libevent-dev \
                          libxml2-dev \
                          libusb-1.0-0-dev \
			  python3 \
			  python3-pip \
			  libblkid-dev \
			  e2fslibs-dev \
			  pkg-config \
			  libnuma-dev \
			  liblz4-tool \
			  flex \
			  bison \
			  xsltproc \
			  clang-format \
			  wget \
			  supervisor \
			  && apt-get clean

RUN pip3 install kconfiglib --proxy=http://child-prc.intel.com:913 \
    && pip3 install lxml --proxy=http://child-prc.intel.com:913 \
    && wget https://acpica.org/sites/acpica/files/acpica-unix-20210105.tar.gz \
    && tar zxvf acpica-unix-20210105.tar.gz \
    && cd acpica-unix-20210105 \
    && make clean \
    && make iasl \
    && cp ./generate/unix/bin/iasl /usr/sbin/

COPY misc/config_tools/config_app/requirements .
RUN pip3 install -r requirements --proxy=http://child-prc.intel.com:913

EXPOSE 5001/udp
EXPOSE 5001/tcp
USER acrn
CMD ["/bin/bash"]

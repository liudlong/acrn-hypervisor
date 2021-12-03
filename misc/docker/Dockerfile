# syntax=docker/dockerfile1.2

ARG UBUNTU_VERSION=20.04
ARG UBUNTU_IMAGE="ubuntu:${UBUNTU_VERSION}"

# Build container based on Ubuntu 20.04
FROM ${UBUNTU_IMAGE} AS base
RUN echo 'Binary::apt::APT::Keep-Downloaded-Packages "true";' > /etc/apt/apt.conf.d/keep-cache
RUN echo "Acquire::http::proxy \"$HTTP_PROXY\";\nAcquire::https::proxy \"$HTTPS_PROXY\";" > /etc/apt/apt.conf
RUN apt-get update
ENV DEBIAN_FRONTEND=noninteractive

FROM base AS ACRN

RUN addgroup --gid 1000 docker && \
    adduser --uid 1000 --ingroup docker --home /home/docker --shell /bin/sh --disabled-password --gecos "" docker

RUN apt-get install curl -y

RUN USER=docker && \
    GROUP=docker && \
    curl -SsL https://github.com/boxboat/fixuid/releases/download/v0.5.1/fixuid-0.5.1-linux-amd64.tar.gz | tar -C /usr/local/bin -xzf - && \
    chown root:root /usr/local/bin/fixuid && \
    chmod 4755 /usr/local/bin/fixuid && \
    mkdir -p /etc/fixuid && \
    printf "user: $USER\ngroup: $GROUP\n" > /etc/fixuid/config.yml

WORKDIR /Workspace
# Install dependencies.
RUN apt-get update \
    && apt-get install -y gcc make vim git \
		libssl-dev \
     		libpciaccess-dev \
		uuid-dev \
	     	libsystemd-dev \
		libevent-dev \
	 	libxml2-dev \
		libxml2-utils \
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
 	        bc \
		wget \
		&& apt-get clean


RUN echo "http_proxy=$HTTP_PROXY\nhttps_proxy=$HTTP_PROXY\nftp_proxy=$HTTP_PROXY" > ~/.wgetrc
RUN pip3 install kconfiglib --proxy=$HTTP_PROXY \
    && pip3 install lxml xmlschema defusedxml --proxy=$HTTP_PROXY \
    && pip3 install lxml --proxy=$HTTP_PROXY \
    && wget https://acpica.org/sites/acpica/files/acpica-unix-20210105.tar.gz \
    && tar zxvf acpica-unix-20210105.tar.gz \
    && cd acpica-unix-20210105 \
    && make clean \
    && make iasl \
    && cp ./generate/unix/bin/iasl /usr/sbin/

COPY requirements .
RUN pip3 install -r requirements --proxy=$HTTP_PROXY

EXPOSE 5001/udp
EXPOSE 5001/tcp
USER docker:docker
ENTRYPOINT ["fixuid"]
CMD ["/bin/bash"]

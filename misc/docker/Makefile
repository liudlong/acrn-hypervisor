# Minimal makefile for Sphinx documentation
#
ACRN_BASE     = "$(CURDIR)/../../"
include ACRN_BASE/VERSION
export FULL_VERSION=$(MAJOR_VERSION).$(MINOR_VERSION)$(EXTRA_VERSION)

DOCKERFILE ?= Dockerfile
DOCKERFILE_ID := $(shell git log --pretty=format:"%h" $(ACRN_BASE)/VERSION | head -1  | awk '{print $1}')
DOCKER_IMAGE_NAME := acrn$(FULL_VERSION):$(DOCKERFILE_ID)

BUILD_HTTP_PROXY := $(if $(HTTP_PROXY), --build-arg HTTP_PROXY=$(HTTP_PROXY))
BUILD_HTTP_PROXY := $(if $(http_proxy), --build-arg HTTP_PROXY=$(http_proxy))
BUILD_HTTPS_PROXY := $(if $(HTTPS_PROXY), --build-arg HTTPS_PROXY=$(HTTPS_PROXY))
BUILD_HTTPS_PROXY := $(if $(https_proxy), --build-arg HTTPS_PROXY=$(https_proxy))

DOCKER_BUILD_OPTS := $(BUILD_HTTP_PROXY) $(BUILD_HTTPS_PROXY)

CURRENT_UID := $(shell id -u)
CURRENT_GID := $(shell id -g)

.PHONY: docker
docker:
	cp $(ACRN_BASE)/misc/config_tools/config_app/requirements .;
	docker build $(DOCKER_BUILD_OPTS) -t $(DOCKER_IMAGE_NAME) -f $(DOCKERFILE) .;
	docker run -it --rm -u $(CURRENT_UID):$(CURRENT_GID) -v $(ACRN_BASE):/Workspace/ $(DOCKER_IMAGE_NAME)


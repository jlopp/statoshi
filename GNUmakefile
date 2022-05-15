SHELL := /bin/bash
PWD 									?= pwd_unknown
THIS_FILE								:= $(lastword $(MAKEFILE_LIST))
export THIS_FILE
TIME									:= $(shell date +%s)
export TIME
ARCH                                    := $(shell uname -m)
export ARCH
ifeq ($(user),)
#DEFAULT to root
HOST_USER								:= root
HOST_UID								:= $(strip $(if $(uid),$(uid),0))
else
HOST_USER								:=  $(strip $(if $(user),$(user),nodummy))
HOST_UID								:=  $(strip $(if $(shell id -u),$(shell id -u),4000))
endif
export HOST_USER
export HOST_UID
ifeq ($(target),)
SERVICE_TARGET							?= statoshi
else
SERVICE_TARGET							:= $(target)
endif
export SERVICE_TARGET
ifeq ($(docker),)
DOCKER									:= $(shell which docker)
else
DOCKER   								:= $(docker)
endif
export DOCKER
ifeq ($(compose),)
DOCKER_COMPOSE							:= $(shell which docker-compose)
else
DOCKER_COMPOSE							:= $(compose)
endif
export DOCKER_COMPOSE
ifeq ($(alpine),)
ALPINE_VERSION							:= 3.11.6
else
ALPINE_VERSION							:= $(alpine)
endif
export ALPINE_VERSION
ifeq ($(project),)
PROJECT_NAME							:= $(notdir $(PWD))
else
PROJECT_NAME							:= $(project)
endif
export PROJECT_NAME
GIT_USER_NAME							:= $(shell git config user.name)
export GIT_USER_NAME
GIT_USER_EMAIL							:= $(shell git config user.email)
export GIT_USER_EMAIL
GIT_SERVER								:= https://github.com
export GIT_SERVER
GIT_REPO_NAME							:= $(PROJECT_NAME)
export GIT_REPO_NAME
ifeq ($(profile),)
#USAGE EXAMPLE: make init profile=jlopp
GIT_PROFILE								:= bitcoincore-dev
else
GIT_PROFILE								:= $(profile)
endif
export GIT_PROFILE
#GIT_BRANCH								:= $(shell git rev-parse --abbrev-ref HEAD)
GIT_BRANCH								:= $(shell git rev-parse --short HEAD~0)
export GIT_BRANCH
GIT_HASH								:= $(shell git rev-parse --short HEAD~0)
export GIT_HASH
GIT_PREVIOUS_HASH						:= $(shell git rev-parse --short master@{1})
export GIT_PREVIOUS_HASH
GIT_REPO_ORIGIN							:= $(shell git remote get-url origin)
export GIT_REPO_ORIGIN
GIT_REPO_PATH							:= $(HOME)/$(GIT_REPO_NAME)
export GIT_REPO_PATH
DOCKERFILE								:= $(PROJECT_NAME).dockerfile
export DOCKERFILE
DOCKERFILE_PATH							:= $(HOME)/$(PROJECT_NAME)/$(DOCKERFILE)
export DOCKERFILE_PATH
BITCOIN_CONF							:= $(HOME)/$(PROJECT_NAME)/conf/bitcoin.conf
export BITCOIN_CONF
ifeq ($(network),)
NETWORK									:= signet
else
NETWORK									:= $(network)
endif
export NETWORK
ifeq ($(datadir),)
DATADIR									:= $(HOME)/.statoshi
else
DATADIR									:= $(datadir)
endif
export DATADIR
ifeq ($(nocache),true)
NOCACHE									:= --no-cache
else
NOCACHE									:=
endif
export NOCACHE
ifeq ($(verbose),true)
VERBOSE									:= --verbose
else
VERBOSE									:=
endif
export VERBOSE
ifeq ($(port),)
PUBLIC_PORT								:= 80
else
PUBLIC_PORT								:= $(port)
endif
export PUBLIC_PORT
ifeq ($(nodeport),)
NODE_PORT								:= 8333
else
NODE_PORT								:= $(nodeport)
endif
export NODE_PORT
ifneq ($(passwd),)
PASSWORD								:= $(passwd)
else
PASSWORD								:= changeme
endif
export PASSWORD
ifeq ($(cmd),)
CMD_ARGUMENTS							:=
else
CMD_ARGUMENTS							:= $(cmd)
endif
export CMD_ARGUMENTS
PACKAGE_PREFIX							:= ghcr.io
export PACKAGE_PREFIX

.PHONY: help
help:
	@echo ''
	@echo '	[USAGE]:	make [BUILD] run [EXTRA_ARGUMENTS]	'
	@echo ''
	@echo '		make init'
	@echo ''
	@echo '	bitcoin commands:'
	@echo ''
	@echo '		make local-autogen'
	@echo '		make local-configure'
	@echo ''
	@echo '	docker commands:'
	@echo ''
	@echo '		make build-docker'
	@echo ''
	@echo '		make init user=root uid=0 nocache=false verbose=true'
	@echo ''
	@echo '	[DEV ENVIRONMENT]:	'
	@echo ''
	@echo '		make build  user=root'
	@echo '		make run    user=root'
	@echo '		make shell  user=$(HOST_USER)'
	@echo ''
	@echo '	[EXTRA_ARGUMENTS]:	set build variables	'
	@echo ''
	@echo '		nocache=true'
	@echo '		            	add --no-cache to docker command and apk add $(NOCACHE)'
	@echo ''
	@echo '		nodeport=integer'
	@echo '		            	set NODE_PORT default 8333'
	@echo ''
	@echo '		            	TODO'
	@echo ''
	@echo '	[DOCKER COMMANDS]:	push a command to the container	'
	@echo ''
	@echo '		cmd=command 	'
	@echo '		cmd="command"	'
	@echo '		             	send CMD_ARGUMENTS to the [TARGET]'
	@echo ''
	@echo '	[EXAMPLE]:'
	@echo ''
	@echo '		make all run user=root uid=0 no-cache=true verbose=true'
	@echo '		make report build run user=root uid=0 no-cache=true verbose=true cmd="top"'
	@echo ''
.PHONY: report
report:
	@echo ''
	@echo '	[ARGUMENTS]	'
	@echo '      args:'
	@echo '        - HOME=${HOME}'
	@echo '        - PWD=${PWD}'
	@echo '        - UMBREL=${UMBREL}'
	@echo '        - THIS_FILE=${THIS_FILE}'
	@echo '        - TIME=${TIME}'
	@echo '        - PACKAGE_PREFIX=${PACKAGE_PREFIX}'
	@echo '        - ARCH=${ARCH}'
	@echo '        - HOST_USER=${HOST_USER}'
	@echo '        - HOST_UID=${HOST_UID}'
	@echo '        - PUBLIC_PORT=${PUBLIC_PORT}'
	@echo '        - NODE_PORT=${NODE_PORT}'
	@echo '        - SERVICE_TARGET=${SERVICE_TARGET}'
	@echo '        - ALPINE_VERSION=${ALPINE_VERSION}'
	@echo '        - PROJECT_NAME=${PROJECT_NAME}'
	@echo '        - DOCKER_BUILD_TYPE=${DOCKER_BUILD_TYPE}'
	@echo '        - DOCKER_COMPOSE=${DOCKER_COMPOSE}'
	@echo '        - DOCKERFILE=${DOCKERFILE}'
	@echo '        - DOCKERFILE_BODY=${DOCKERFILE_BODY}'
	@echo '        - GIT_USER_NAME=${GIT_USER_NAME}'
	@echo '        - GIT_USER_EMAIL=${GIT_USER_EMAIL}'
	@echo '        - GIT_SERVER=${GIT_SERVER}'
	@echo '        - GIT_PROFILE=${GIT_PROFILE}'
	@echo '        - GIT_BRANCH=${GIT_BRANCH}'
	@echo '        - GIT_HASH=${GIT_HASH}'
	@echo '        - GIT_PREVIOUS_HASH=${GIT_PREVIOUS_HASH}'
	@echo '        - GIT_REPO_ORIGIN=${GIT_REPO_ORIGIN}'
	@echo '        - GIT_REPO_NAME=${GIT_REPO_NAME}'
	@echo '        - GIT_REPO_PATH=${GIT_REPO_PATH}'
	@echo '        - DOCKERFILE=${DOCKERFILE}'
	@echo '        - DOCKERFILE_PATH=${DOCKERFILE_PATH}'
	@echo '        - BITCOIN_CONF=${BITCOIN_CONF}'
	@echo '        - NETWORK=${NETWORK}'
	@echo '        - DATADIR=${DATADIR}'
	@echo '        - NOCACHE=${NOCACHE}'
	@echo '        - VERBOSE=${VERBOSE}'
	@echo '        - NODE_PORT=${NODE_PORT}'
	@echo '        - PASSWORD=${PASSWORD}'
	@echo '        - CMD_ARGUMENTS=${CMD_ARGUMENTS}'

#######################

ORIGIN_DIR:=$(PWD)
MACOS_TARGET_DIR:=/var/root/$(PROJECT_NAME)
LINUX_TARGET_DIR:=/root/$(PROJECT_NAME)
export ORIGIN_DIR
export TARGET_DIR

.PHONY: super
super:
ifneq ($(shell id -u),0)
	@echo switch to superuser
	@echo cd $(TARGET_DIR)
	#sudo ln -s $(PWD) $(TARGET_DIR)
#.ONESHELL:
	sudo -s
endif
#######################
#######################
# Backup $HOME/.bitcoin
########################
#backup:
#	@echo ''
#	bash -c 'mkdir -p $(HOME)/.bitcoin'
##	bash -c 'conf/get_size.sh'
#	bash -c 'tar czv --exclude=*.log --exclude=banlist.dat \
#			--exclude=fee_exstimates.dat --exclude=mempool.dat \
#			--exclude=peers.dat --exclude=.cookie --exclude=database \
#			--exclude=.lock --exclude=.walletlock --exclude=.DS_Store\
#			-f $(HOME)/.bitcoin-$(TIME).tar.gz $(HOME)/.bitcoin'
#	bash -c 'openssl md5 $(HOME)/.bitcoin-$(TIME).tar.gz > $(HOME)/bitcoin-$(TIME).tar.gz.md5'
#	bash -c 'openssl md5 -c $(HOME)/bitcoin-$(TIME).tar.gz.md5'
#
#
#	@echo ''
#######################
# Some initial setup
########################
#######################

.PHONY: host
host:
	@echo 'host'
	bash -c './host'

#######################
.PHONY: init
.SILENT:
init:
	git config --global core.editor vim
	docker pull docker.io/graphiteapp/graphite-statsd:latest
	docker pull ghcr.io/bitcoincore-dev/statoshi/$(ARCH)/root:latest
ifneq ($(shell id -u),0)
	sudo bash -c 'mkdir -p /usr/local/include/'
	sudo bash -c 'install -v $(PWD)/src/statsd_client.h		    /usr/local/include/statsd_client.h'
	sudo bash -c 'install -v $(PWD)/src/statsd_client.cpp		/usr/local/include/statsd_client.cpp'
endif
	rm -f Makefile
#######################
.PHONY: local-autogen
local-autogen:
	./autogen.sh
.PHONY: local-configure
local-configure:
	./configure  --disable-wallet --disable-tests --disable-hardening --disable-man --enable-util-cli --enable-util-tx --with-gui=no --disable-bench
.PHONY: signin
signin:
	bash -c 'cat ~/GH_TOKEN.txt | docker login ghcr.io -u RandyMcMillan --password-stdin'
	docker tag $(PROJECT_NAME):$(HOST_USER) $(PACKAGE_PREFIX)/$(GIT_PROFILE)/$(PROJECT_NAME)/$(ARCH)/$(HOST_USER)

.PHONY: build build-docker
build: build-docker
build-docker:
	@echo 'build'
	#$(DOCKER_COMPOSE) $(VERBOSE) build --pull $(NOCACHE) statoshi
	$(DOCKER_COMPOSE) $(VERBOSE) build $(NOCACHE) statoshi
	@echo ''
#######################
.PHONY: run
run: init
	@echo 'run'
ifeq ($(CMD_ARGUMENTS),)
	@echo ''
	$(DOCKER_COMPOSE) $(VERBOSE) -p $(PROJECT_NAME)_$(HOST_UID) run -d statoshi sh
	#$(DOCKER_COMPOSE) $(VERBOSE) -p $(PROJECT_NAME)_$(HOST_UID) run -d --publish 8125:8125 --publish 8126:8126 --publish 8333:8333 --publish 8332:8332 statoshi sh
	@echo ''
else
	@echo ''
	$(DOCKER_COMPOSE) $(VERBOSE) -p $(PROJECT_NAME)_$(HOST_UID) run -d statoshi sh -c "$(CMD_ARGUMENTS)"
	#$(DOCKER_COMPOSE) $(VERBOSE) -p $(PROJECT_NAME)_$(HOST_UID) run -d --publish 8125:8125 --publish 8126:8126 --publish 8333:8333 --publish 8332:8332 statoshi sh -c "$(CMD_ARGUMENTS)"
	@echo ''
endif
#######################
.PHONY: torproxy
torproxy:
	@echo ''
	#REF: https://hub.docker.com/r/dperson/torproxy
	bash -c 'docker run -it -p 8118:8118 -p 9050:9050 -p 9051:9051 -d dperson/torproxy'
	@echo ''
ifneq ($(shell id -u),0)
	bash -c 'sudo make torproxy user=root &'
endif
ifeq ($(CMD_ARGUMENTS),)
	$(DOCKER_COMPOSE) $(VERBOSE) -f docker-compose.yml -p $(PROJECT_NAME)_$(HOST_UID) run --publish 8118:8118 --publish 9050:9050  --publish 9051:9051 --rm torproxy
else
	$(DOCKER_COMPOSE) $(VERBOSE) -f docker-compose.yml -p $(PROJECT_NAME)_$(HOST_UID) run --publish 8118:8118 --publish 9050:9050  --publish 9051:9051 --rm torproxy sh -c "$(CMD_ARGUMENTS)"
endif
	@echo ''
#######################
.PHONY: clean
clean:
	# remove created images
	@$(DOCKER_COMPOSE) -p $(PROJECT_NAME)_$(HOST_UID) down --remove-orphans --rmi all 2>/dev/null \
	&& echo 'Image(s) for "$(PROJECT_NAME):$(HOST_USER)" removed.' \
	|| echo 'Image(s) for "$(PROJECT_NAME):$(HOST_USER)" already removed.'
	@rm -f Makefile
#######################
.PHONY: prune
prune:
	@echo 'prune'
	$(DOCKER_COMPOSE) -p $(PROJECT_NAME)_$(HOST_UID) down
	docker system prune -f
#######################
.PHONY: prune-network
prune-network:
	@echo 'prune-network'
	$(DOCKER_COMPOSE) -p $(PROJECT_NAME)_$(HOST_UID) down
	docker network prune -f
#######################
.PHONY: docs
docs:
#$ make report no-cache=true verbose=true cmd='make doc' user=root doc
#SHELL := /bin/bash
	@echo 'docs'
	#bash -c "if pgrep MacDown; then pkill MacDown; fi"
	bash -c "touch $(PWD)/docker/README.md"
	curl -o $(PWD)/docker/README.md  https://raw.githubusercontent.com/jlopp/statoshi/master/README.md
	bash -c "cat $(PWD)/docker/README.md"
	bash -c "cat $(PWD)/docker/README.md >  README.md"
	bash -c "echo ' ' >> README.md"
	#bash -c "echo '<insert string>' >> README.md"
	bash -c "echo '----' >> README.md"
	bash -c "cat $(PWD)/docker/DOCKER.md >> README.md"
	#bash -c "echo '## [$(PROJECT_NAME)]($(GIT_SERVER)/$(GIT_PROFILE)/$(PROJECT_NAME)) ' >> README.md"
	bash -c "echo '##### &#36; <code>make</code>' >> README.md"
	bash -c "make help >> README.md"
	#bash -c "if hash open 2>/dev/null; then open README.md; fi || echo failed to open README.md"
.PHONY: push
push:
	@echo 'push'
	#bash -c "git reset --soft HEAD~1 || echo failed to add docs..."
	#bash -c "git add README.md docker/README.md docker/DOCKER.md *.md docker/*.md || echo failed to add docs..."
	#bash -c "git commit --amend --no-edit --allow-empty -m '$(GIT_HASH)'          || echo failed to commit --amend --no-edit"
	#bash -c "git commit         --no-edit --allow-empty -m '$(GIT_PREVIOUS_HASH)' || echo failed to commit --amend --no-edit"
	bash -c "git push -f --all git@github.com:$(GIT_PROFILE)/$(PROJECT_NAME).git || echo failed to push docs"
.PHONY: push-docs
push-docs: docs push
	@echo 'push-docs'
#######################
package-statoshi: init signin
	#@echo "legit . -m "$(HOST_USER):$(TIME)" -p 0000000 && make user=root package && GPF"
	bash -c 'cat ~/GH_TOKEN.txt | docker login ghcr.io -u RandyMcMillan --password-stdin'
	bash -c 'docker tag $(PROJECT_NAME):$(HOST_USER) ghcr.io/$(GIT_PROFILE)/$(PROJECT_NAME)/$(ARCH)/$(HOST_USER):$(TIME)'
	bash -c 'docker push                             ghcr.io/$(GIT_PROFILE)/$(PROJECT_NAME)/$(ARCH)/$(HOST_USER):$(TIME)'
	bash -c 'docker tag $(PROJECT_NAME):$(HOST_USER) ghcr.io/$(GIT_PROFILE)/$(PROJECT_NAME)/$(ARCH)/$(HOST_USER)'
	bash -c 'docker push                             ghcr.io/$(GIT_PROFILE)/$(PROJECT_NAME)/$(ARCH)/$(HOST_USER)'
#
#	touch TIME && echo $(TIME) > TIME && git add -f TIME
#	#legit . -m "make package-header at $(TIME)" -p 00000
#	git commit --amend --no-edit --allow-empty
#	bash -c 'docker tag $(PROJECT_NAME):$(HOST_USER) $(PACKAGE_PREFIX)/$(GIT_PROFILE)/$(PROJECT_NAME)/$(ARCH)/$(HOST_USER):$(TIME)'
#	bash -c 'docker push                             $(PACKAGE_PREFIX)/$(GIT_PROFILE)/$(PROJECT_NAME)/$(ARCH)/$(HOST_USER):$(TIME)'
#	bash -c 'docker tag $(PROJECT_NAME):$(HOST_USER) $(PACKAGE_PREFIX)/$(GIT_PROFILE)/$(PROJECT_NAME)/$(ARCH)/$(HOST_USER)' #defaults to latest
#	bash -c 'docker push                             $(PACKAGE_PREFIX)/$(GIT_PROFILE)/$(PROJECT_NAME)/$(ARCH)/$(HOST_USER)'

########################
.PHONY: package-build-all
package-build-all: init signin package-statoshi

	make build-docker package-statoshi

########################
.PHONY: automate
automate:
	./.github/workflows/automate.sh
#-include funcs.mk
-include Makefile


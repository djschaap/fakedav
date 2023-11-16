# fusedav uses a multi-stage image build process.
#
# base adds some basic packages to fedora:28.  It is separate to allow
# layer caching to operate.
#
# dev includes the compiler and development libraries.  It is the terminal
# stage used when running a Dev Container.
#
# compile executes the actual compile operation.
#
# extract builds an image containing only the RPM.
# By exporting that layer (BuildKit feature), we are able to access the RPM
# from the host for later publishing.
#
# runtime is the final runtime image.
#
FROM docker.io/library/fedora:28 AS base

SHELL ["/bin/bash", "-euo", "pipefail", "-c"]

RUN \
  dnf install -y \
    fuse \
    fuse-libs \
    jemalloc \
    leveldb \
    sudo \
    uriparser \
    which \
  && dnf clean all \
  && rm -rf /var/cache/dnf \
  && groupadd -g 1098 fusedav \
  && useradd -u 1098 -g 1098 -G wheel -d /home/fusedav -s /bin/bash -m fusedav \
  && groupadd -g 1099 vscode \
  && useradd -u 1099 -g 1099 -G wheel -d /home/vscode -s /bin/bash -m vscode \
  && echo "%wheel ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/wheel \
  && grep -E -q '^user_allow_other' /etc/fuse.conf || echo user_allow_other >> /etc/fuse.conf

# TODO: consider removing fusedav from wheel group

########################################

FROM base AS dev

RUN \
  dnf install -y \
    'dnf-command(config-manager)' \
    autoconf \
    automake \
    bind-utils \
    expat-devel \
    findutils \
    fuse-devel \
    gcc \
    gdb \
    git \
    glib2-devel \
    jemalloc-devel \
    leveldb-devel \
    libcurl-devel \
    make \
    procps-ng \
    rpm-build \
    ruby-devel \
    strace \
    systemd-devel \
    tcpdump \
    uriparser-devel \
    zlib-devel \
  && sudo dnf config-manager --add-repo https://cli.github.com/packages/rpm/gh-cli.repo \
  && sudo dnf install -y gh \
  && dnf clean all \
  && rm -rf /var/cache/dnf \
  && gem install fpm --no-rdoc --no-ri \
  && gem install package_cloud -v 0.2.45 \
  && curl -fsSL https://github.com/pantheon-systems/autotag/releases/latest/download/autotag_linux_amd64 \
    -o /usr/local/bin/autotag \
  && chmod 0755 /usr/local/bin/autotag

# Installing autotag above makes it available within a dev container.
# When building via CI/CD, autotag is installed/called elsewhere.

# Installing gh above makes it available within a dev container.
# When building via GitHub Actions, gh is installed/called elsewhere.

USER vscode

########################################

FROM dev AS compile

# new-version.sh MUST be created before we get here
COPY . /build
WORKDIR /build

RUN \
  sudo chown -R vscode /build \
  && scripts/build-rpm.sh

########################################

FROM scratch AS extract

COPY --from=compile /home/vscode/rpmbuild/RPMS RPMS
COPY --from=compile /home/vscode/rpmbuild/SRPMS SRPMS
COPY --from=compile /build/LATEST-RPM-VER-REL LATEST-RPM-VER-REL

########################################

FROM base AS runtime

COPY --from=compile \
  /build/LATEST-RPM-VER-REL \
  /home/vscode/rpmbuild/RPMS/x86_64/fusedav-*.rpm \
  /tmp/

RUN \
  LATEST=$(cat /tmp/LATEST-RPM-VER-REL) \
  && sudo rpm -i "/tmp/fusedav-${LATEST}.x86_64.rpm"

USER fusedav

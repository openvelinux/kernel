FROM debian:11.0
RUN echo deb [trusted=yes] http://mirrors.byted.org/debian/ bullseye main contrib > /etc/apt/sources.list
RUN echo deb [trusted=yes] http://mirrors.byted.org/debian/ bullseye-backports main contrib >> /etc/apt/sources.list
RUN apt-get update && apt-get install -y gnupg2
RUN echo deb http://apt.byted.org/private bullseye-private bullseye-main > /etc/apt/sources.list.d/private.list
ADD private-apt-key /tmp/key
ADD debian-packages.txt /tmp/debian-packages.txt
RUN apt-key add /tmp/key
RUN apt-get update --allow-releaseinfo-change -y && \
    apt-get upgrade -y && \
    apt-get install -y $(cat /tmp/debian-packages.txt)

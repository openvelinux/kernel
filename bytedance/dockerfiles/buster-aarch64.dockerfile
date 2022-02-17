FROM debian:10.0
RUN echo deb [trusted=yes] http://mirrors.byted.org/debian/ buster main contrib > /etc/apt/sources.list
RUN echo deb [trusted=yes] http://mirrors.byted.org/debian/ buster-backports main contrib >> /etc/apt/sources.list
run apt-get update && apt-get install -y gnupg2
ADD private-apt-key /tmp/key
ADD debian-packages.txt /tmp/debian-packages.txt
RUN apt-key add /tmp/key
RUN apt-get update --allow-releaseinfo-change -y && \
    apt-get upgrade -y && \
    apt-get install -y $(cat /tmp/debian-packages.txt)

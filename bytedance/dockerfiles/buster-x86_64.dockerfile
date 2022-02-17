FROM gaea-hub.byted.org/mirror/debian:10.0
RUN echo deb [trusted=yes] http://mirrors.byted.org/debian/ buster main contrib > /etc/apt/sources.list
RUN echo deb [trusted=yes] http://mirrors.byted.org/debian/ buster-backports main contrib >> /etc/apt/sources.list
RUN apt-get update && apt-get install -y gnupg2
RUN echo deb http://apt.byted.org/private buster-private buster-main > /etc/apt/sources.list.d/private.list
RUN echo Package: dwarves > /etc/apt/preferences.d/dwarves
RUN echo Pin: release o=. buster-private, a=buster-private >> /etc/apt/preferences.d/dwarves
RUN echo Pin-Priority: 1001 >> /etc/apt/preferences.d/dwarves
ADD private-apt-key /tmp/key
ADD debian-packages.txt /tmp/debian-packages.txt
RUN apt-key add /tmp/key
RUN apt-get update --allow-releaseinfo-change -y && \
    apt-get upgrade -y && \
    apt-get install -y $(cat /tmp/debian-packages.txt)

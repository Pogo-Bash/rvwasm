# syntax=docker/dockerfile:1
# Standalone runtime image; the release dist is supplied at /dist when the container runs.
FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
		python3 iproute2 iptables util-linux ca-certificates curl \
		nftables dnsmasq-base procps \
	&& rm -rf /var/lib/apt/lists/* \
	&& useradd --system --create-home --shell /usr/sbin/nologin rv

COPY tools/net-relay.py tools/net-setup.sh docker/relay.sh docker/net-allowlist.sh \
	docker/dnsmasq-conf.sh docker/allowlist-reload.sh docker/serve.py docker/fetch.sh /opt/rvwasm/
COPY docker/allowlist.txt /etc/rvwasm/allowlist.txt
COPY site/ /site/

EXPOSE 8000 8001
CMD ["setpriv", "--reuid=rv", "--regid=rv", "--init-groups", "python3", \
	"/opt/rvwasm/serve.py", "--dist", "/dist", "--site", "/site", "--port", "8000"]
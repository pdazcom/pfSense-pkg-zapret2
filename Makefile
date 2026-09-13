PLUGIN_DIR != pwd

# Deploy to a remote pfSense box.
# Usage: make install HOST=root@10.0.0.1
HOST ?=
SSH_OPTS ?=

ZAPRET2_VERSION ?= v0.9.5.2
ZAPRET2_TARBALL = /tmp/zapret2-$(ZAPRET2_VERSION).tar.gz
ZAPRET2_URL = https://github.com/bol-van/zapret2/releases/download/$(ZAPRET2_VERSION)/zapret2-$(ZAPRET2_VERSION).tar.gz

# Local path to ipset file from zapret-discord-youtube (updated on Windows/local machine)
IPSET_LOCAL ?= $(HOME)/Downloads/zapret-discord-youtube/.service/ipset-service.txt
IPSET_REMOTE = /usr/local/etc/zapret2/ipset-all.txt

# Download zapret2 tarball locally if not already cached
$(ZAPRET2_TARBALL):
	@echo "Downloading zapret2 $(ZAPRET2_VERSION)..."
	curl -fsSL "$(ZAPRET2_URL)" -o "$(ZAPRET2_TARBALL)"
	@echo "Saved to $(ZAPRET2_TARBALL)"

install: $(ZAPRET2_TARBALL)
	@if [ -n "$(HOST)" ]; then \
		echo "=== Deploying to $(HOST) ==="; \
		ssh $(HOST) 'mkdir -p /tmp/pfSense-pkg-zapret2'; \
		scp -r files scripts install.sh "$(ZAPRET2_TARBALL)" $(HOST):/tmp/pfSense-pkg-zapret2/; \
		ssh $(HOST) 'cd /tmp/pfSense-pkg-zapret2 && sh install.sh --tarball /tmp/pfSense-pkg-zapret2/$(notdir $(ZAPRET2_TARBALL))'; \
	else \
		echo "=== Installing locally ==="; \
		sh install.sh; \
	fi

uninstall:
	@if [ -n "$(HOST)" ]; then \
		echo "=== Uninstalling from $(HOST) ==="; \
		scp uninstall.sh $(HOST):/tmp/zapret2-uninstall.sh; \
		ssh $(HOST) 'sh /tmp/zapret2-uninstall.sh $(UNINSTALL_ARGS); rm -f /tmp/zapret2-uninstall.sh'; \
	else \
		echo "=== Uninstalling locally ==="; \
		sh uninstall.sh $(UNINSTALL_ARGS); \
	fi

# Push only plugin files (no backend re-download). Useful during development.
# All files are staged into a temp tree, tarred, and sent in one SSH connection — one password prompt.
update:
	@if [ -z "$(HOST)" ]; then echo "Usage: make update HOST=root@<ip>"; exit 1; fi
	@echo "=== Updating plugin files on $(HOST) ==="
	@STAGE=$$(mktemp -d /tmp/zapret2-update.XXXXXX); \
	mkdir -p \
		"$$STAGE/usr/local/www/zapret2" \
		"$$STAGE/usr/local/pkg/zapret2/includes" \
		"$$STAGE/usr/local/etc/rc.d" \
		"$$STAGE/usr/local/etc/zapret2" \
		"$$STAGE/usr/local/share/zapret2"; \
	cp files/usr/local/www/zapret2/zapret2.php                    "$$STAGE/usr/local/www/zapret2/zapret2.php"; \
	cp files/usr/local/www/zapret2/zapret2_test_runner.php        "$$STAGE/usr/local/www/zapret2/zapret2_test_runner.php"; \
	cp files/usr/local/pkg/zapret2/includes/zapret2.inc  "$$STAGE/usr/local/pkg/zapret2/includes/zapret2.inc"; \
	cp files/usr/local/etc/rc.d/zapret2                  "$$STAGE/usr/local/etc/rc.d/zapret2"; \
	cp files/usr/local/etc/zapret2/profiles.conf         "$$STAGE/usr/local/etc/zapret2/profiles.conf"; \
	cp scripts/healthcheck.sh                            "$$STAGE/usr/local/share/zapret2/healthcheck.sh"; \
	cp scripts/configtest.sh                             "$$STAGE/usr/local/share/zapret2/configtest.sh"; \
	COPYFILE_DISABLE=1 tar -cf - -C "$$STAGE" usr 2>/dev/null \
	| ssh $(SSH_OPTS) $(HOST) 'tar -xf - -C / --no-same-owner 2>/dev/null; \
		chmod 555  /usr/local/etc/rc.d/zapret2 && \
		chmod 644  /usr/local/www/zapret2/zapret2.php \
		           /usr/local/www/zapret2/zapret2_test_runner.php \
		           /usr/local/pkg/zapret2/includes/zapret2.inc \
		           /usr/local/etc/zapret2/profiles.conf && \
		chmod 755  /usr/local/share/zapret2/healthcheck.sh \
		           /usr/local/share/zapret2/configtest.sh && \
		echo Done.'; \
	rm -rf "$$STAGE"

check:
	@echo "=== PHP lint ==="
	php -l files/usr/local/www/zapret2/zapret2.php
	php -l files/usr/local/pkg/zapret2/includes/zapret2.inc
	@echo "=== Shell lint ==="
	sh -n files/usr/local/etc/rc.d/zapret2
	sh -n scripts/healthcheck.sh
	sh -n scripts/configtest.sh
	sh -n install.sh
	sh -n uninstall.sh
	@echo "=== XML ==="
	xmllint --noout pkg/zapret2.xml 2>/dev/null && echo "zapret2.xml OK" || true
	@echo "=== All checks passed ==="

.PHONY: install uninstall update check

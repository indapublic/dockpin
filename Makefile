# DockPin. `make help` lists the commands.
CFLAGS ?= -O2 -Wall
FRAMEWORKS = -framework CoreGraphics -framework CoreFoundation
# Ad hoc by default; a real identity also gets the hardened runtime and a
# timestamp, which notarization requires.
SIGN ?= -
CODESIGN_FLAGS = $(if $(filter -,$(SIGN)),,--options runtime --timestamp)
TAP = indapublic/dockpin
TAP_URL = https://github.com/indapublic/dockpin
FORMULA = $(TAP)/dockpin

build: DockPin.app ## Build DockPin.app (default)

DockPin.app: dockpin.c Info.plist AppIcon.icns
	rm -rf $@
	mkdir -p $@/Contents/MacOS $@/Contents/Resources
	cp Info.plist $@/Contents/Info.plist
	cp AppIcon.icns $@/Contents/Resources/AppIcon.icns
	$(CC) $(CFLAGS) -o $@/Contents/MacOS/DockPin dockpin.c $(FRAMEWORKS)
	codesign --force --sign "$(SIGN)" $(CODESIGN_FLAGS) $@

AppIcon.icns: $(wildcard assets/AppIcon.iconset/*.png)
	iconutil -c icns -o $@ assets/AppIcon.iconset

icon: ## Render the icon PNGs from the SVGs (needs Google Chrome)
	tools/render-icon.sh

tests/edge_test: tests/edge_test.c dockpin.c
	$(CC) $(CFLAGS) -o $@ tests/edge_test.c $(FRAMEWORKS)

test: tests/edge_test ## Run the edge-logic tests
	./tests/edge_test

lint: ## Run the clang static analyzer and shellcheck
	$(CC) --analyze -Xanalyzer -analyzer-output=text -Xanalyzer -analyzer-werror -o /dev/null dockpin.c
	@command -v shellcheck >/dev/null || { echo "shellcheck is missing: brew install shellcheck"; exit 1; }
	shellcheck tools/*.sh

install: ## Install from the Homebrew tap and start the service
	brew tap $(TAP) $(TAP_URL)
	brew trust --formula $(FORMULA)
	brew install $(FORMULA)
	brew services start dockpin
	@echo "Now enable DockPin in System Settings > Privacy & Security > Accessibility."

update: ## Upgrade to the latest release and restart the service
	brew update
	brew upgrade dockpin
	brew services restart dockpin

uninstall: ## Stop the service and uninstall
	-brew services stop dockpin
	brew uninstall dockpin
	@echo "Now remove DockPin in System Settings > Privacy & Security > Accessibility (- button)."

start: ## Start the service
	brew services start dockpin

stop: ## Stop the service
	brew services stop dockpin

restart: ## Restart the service
	brew services restart dockpin

status: ## Show the service state and memory footprint
	@brew services info dockpin
	@footprint DockPin 2>/dev/null | grep -o 'Footprint: .*' || true

logs: ## Follow the service log
	tail -f "$$(brew --prefix)/var/log/dockpin.log"

release: ## Release a new version: make release VERSION=x.y.z
	@test -n "$(VERSION)" || { echo "Usage: make release VERSION=x.y.z"; exit 2; }
	tools/release.sh $(VERSION)

clean: ## Remove build products
	rm -rf DockPin.app AppIcon.icns tests/edge_test

help: ## List the commands
	@grep -E '^[a-z]+:.*## ' $(MAKEFILE_LIST) | awk -F ':.*## ' '{ printf "  make %-10s %s\n", $$1, $$2 }'

.PHONY: build icon test lint install update uninstall start stop restart status logs release clean help

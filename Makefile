TARGETS = jamendo-fuse

.PHONY: all $(TARGETS)
all: $(TARGETS)

MAKE_OPTS = --no-print-directory V=$V

.PHONY: jamendo-fuse
jamendo-fuse:
	@echo -e "Building: jamendo-fuse"
	@$(MAKE) $(MAKE_OPTS) -C src/

.PHONY: rpm
rpm:
	@echo -e "Building: rpm"
ifeq ($(wildcard ~/rpmbuild/),)
	@echo "***"
	@echo "*** ~/rpmbuild not found, create with"
	@echo "***"
	@echo "***    mkdir -p ~/rpmbuild/{BUILD,RPMS,SOURCES,SPECS,SRPMS}"
	@echo "***"
	@false
else
	@version=$$(git describe | tail -c +2); echo "Building $${version}"; \
		git archive --prefix=jamendo-fuse-$${version%%-*}/ -o ~/rpmbuild/SOURCES/jamendo-fuse-$${version%%-*}.tar HEAD; \
		git describe | tail -c +2 > .version; \
		tar -rf ~/rpmbuild/SOURCES/jamendo-fuse-$${version%%-*}.tar --transform "s,^,jamendo-fuse-$${version%%-*}/," .version
	@rpmbuild -bb jamendo-fuse.spec
endif

.PHONY: clean
clean:
	@echo -e "Cleaning: $(TARGETS)"
	@$(MAKE) $(MAKE_OPTS) -C src/ clean
	@rm -f .version

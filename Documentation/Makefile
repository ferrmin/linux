# -*- makefile -*-
# Makefile for Sphinx documentation
#

# for cleaning
subdir- := devicetree/bindings

ifneq ($(MAKECMDGOALS),cleandocs)
# Check for broken documentation file references
ifeq ($(CONFIG_WARN_MISSING_DOCUMENTS),y)
$(shell $(srctree)/scripts/documentation-file-ref-check --warn)
endif

# Check for broken ABI files
ifeq ($(CONFIG_WARN_ABI_ERRORS),y)
$(shell $(srctree)/scripts/get_abi.py --dir $(srctree)/Documentation/ABI validate)
endif
endif

# You can set these variables from the command line.
SPHINXBUILD   = sphinx-build
SPHINXOPTS    =
SPHINXDIRS    = .
DOCS_THEME    =
DOCS_CSS      =
_SPHINXDIRS   = $(sort $(patsubst $(srctree)/Documentation/%/index.rst,%,$(wildcard $(srctree)/Documentation/*/index.rst)))
SPHINX_CONF   = conf.py
PAPER         =
BUILDDIR      = $(obj)/output
PDFLATEX      = xelatex
LATEXOPTS     = -interaction=batchmode -no-shell-escape

# For denylisting "variable font" files
# Can be overridden by setting as an env variable
FONTS_CONF_DENY_VF ?= $(HOME)/deny-vf

ifeq ($(findstring 1, $(KBUILD_VERBOSE)),)
SPHINXOPTS    += "-q"
endif

# User-friendly check for sphinx-build
HAVE_SPHINX := $(shell if which $(SPHINXBUILD) >/dev/null 2>&1; then echo 1; else echo 0; fi)

ifeq ($(HAVE_SPHINX),0)

.DEFAULT:
	$(warning The '$(SPHINXBUILD)' command was not found. Make sure you have Sphinx installed and in PATH, or set the SPHINXBUILD make variable to point to the full path of the '$(SPHINXBUILD)' executable.)
	@echo
	@$(srctree)/scripts/sphinx-pre-install
	@echo "  SKIP    Sphinx $@ target."

else # HAVE_SPHINX

# User-friendly check for pdflatex and latexmk
HAVE_PDFLATEX := $(shell if which $(PDFLATEX) >/dev/null 2>&1; then echo 1; else echo 0; fi)
HAVE_LATEXMK := $(shell if which latexmk >/dev/null 2>&1; then echo 1; else echo 0; fi)

ifeq ($(HAVE_LATEXMK),1)
	PDFLATEX := latexmk -$(PDFLATEX)
endif #HAVE_LATEXMK

# Internal variables.
PAPEROPT_a4     = -D latex_paper_size=a4
PAPEROPT_letter = -D latex_paper_size=letter
ALLSPHINXOPTS   = -D kerneldoc_srctree=$(srctree) -D kerneldoc_bin=$(KERNELDOC)
ALLSPHINXOPTS   += $(PAPEROPT_$(PAPER)) $(SPHINXOPTS)
ifneq ($(wildcard $(srctree)/.config),)
ifeq ($(CONFIG_RUST),y)
	# Let Sphinx know we will include rustdoc
	ALLSPHINXOPTS   +=  -t rustdoc
endif
endif
# the i18n builder cannot share the environment and doctrees with the others
I18NSPHINXOPTS  = $(PAPEROPT_$(PAPER)) $(SPHINXOPTS) .

# commands; the 'cmd' from scripts/Kbuild.include is not *loopable*
loop_cmd = $(echo-cmd) $(cmd_$(1)) || exit;

# $2 sphinx builder e.g. "html"
# $3 name of the build subfolder / e.g. "userspace-api/media", used as:
#    * dest folder relative to $(BUILDDIR) and
#    * cache folder relative to $(BUILDDIR)/.doctrees
# $4 dest subfolder e.g. "man" for man pages at userspace-api/media/man
# $5 reST source folder relative to $(src),
#    e.g. "userspace-api/media" for the linux-tv book-set at ./Documentation/userspace-api/media

PYTHONPYCACHEPREFIX ?= $(abspath $(BUILDDIR)/__pycache__)

quiet_cmd_sphinx = SPHINX  $@ --> file://$(abspath $(BUILDDIR)/$3/$4)
      cmd_sphinx = $(MAKE) BUILDDIR=$(abspath $(BUILDDIR)) $(build)=Documentation/userspace-api/media $2 && \
	PYTHONPYCACHEPREFIX="$(PYTHONPYCACHEPREFIX)" \
	BUILDDIR=$(abspath $(BUILDDIR)) SPHINX_CONF=$(abspath $(src)/$5/$(SPHINX_CONF)) \
	$(PYTHON3) $(srctree)/scripts/jobserver-exec \
	$(CONFIG_SHELL) $(srctree)/Documentation/sphinx/parallel-wrapper.sh \
	$(SPHINXBUILD) \
	-b $2 \
	-c $(abspath $(src)) \
	-d $(abspath $(BUILDDIR)/.doctrees/$3) \
	-D version=$(KERNELVERSION) -D release=$(KERNELRELEASE) \
	$(ALLSPHINXOPTS) \
	$(abspath $(src)/$5) \
	$(abspath $(BUILDDIR)/$3/$4) && \
	if [ "x$(DOCS_CSS)" != "x" ]; then \
		cp $(if $(patsubst /%,,$(DOCS_CSS)),$(abspath $(srctree)/$(DOCS_CSS)),$(DOCS_CSS)) $(BUILDDIR)/$3/_static/; \
	fi

YNL_INDEX:=$(srctree)/Documentation/networking/netlink_spec/index.rst
YNL_RST_DIR:=$(srctree)/Documentation/networking/netlink_spec
YNL_YAML_DIR:=$(srctree)/Documentation/netlink/specs
YNL_TOOL:=$(srctree)/tools/net/ynl/pyynl/ynl_gen_rst.py

YNL_RST_FILES_TMP := $(patsubst %.yaml,%.rst,$(wildcard $(YNL_YAML_DIR)/*.yaml))
YNL_RST_FILES := $(patsubst $(YNL_YAML_DIR)%,$(YNL_RST_DIR)%, $(YNL_RST_FILES_TMP))

$(YNL_INDEX): $(YNL_RST_FILES)
	$(Q)$(YNL_TOOL) -o $@ -x

$(YNL_RST_DIR)/%.rst: $(YNL_YAML_DIR)/%.yaml $(YNL_TOOL)
	$(Q)$(YNL_TOOL) -i $< -o $@

htmldocs texinfodocs latexdocs epubdocs xmldocs: $(YNL_INDEX)

htmldocs:
	@$(srctree)/scripts/sphinx-pre-install --version-check
	@+$(foreach var,$(SPHINXDIRS),$(call loop_cmd,sphinx,html,$(var),,$(var)))

# If Rust support is available and .config exists, add rustdoc generated contents.
# If there are any, the errors from this make rustdoc will be displayed but
# won't stop the execution of htmldocs

ifneq ($(wildcard $(srctree)/.config),)
ifeq ($(CONFIG_RUST),y)
	$(Q)$(MAKE) rustdoc || true
endif
endif

texinfodocs:
	@$(srctree)/scripts/sphinx-pre-install --version-check
	@+$(foreach var,$(SPHINXDIRS),$(call loop_cmd,sphinx,texinfo,$(var),texinfo,$(var)))

# Note: the 'info' Make target is generated by sphinx itself when
# running the texinfodocs target define above.
infodocs: texinfodocs
	$(MAKE) -C $(BUILDDIR)/texinfo info

linkcheckdocs:
	@$(foreach var,$(SPHINXDIRS),$(call loop_cmd,sphinx,linkcheck,$(var),,$(var)))

latexdocs:
	@$(srctree)/scripts/sphinx-pre-install --version-check
	@+$(foreach var,$(SPHINXDIRS),$(call loop_cmd,sphinx,latex,$(var),latex,$(var)))

ifeq ($(HAVE_PDFLATEX),0)

pdfdocs:
	$(warning The '$(PDFLATEX)' command was not found. Make sure you have it installed and in PATH to produce PDF output.)
	@echo "  SKIP    Sphinx $@ target."

else # HAVE_PDFLATEX

pdfdocs: DENY_VF = XDG_CONFIG_HOME=$(FONTS_CONF_DENY_VF)
pdfdocs: latexdocs
	@$(srctree)/scripts/sphinx-pre-install --version-check
	$(foreach var,$(SPHINXDIRS), \
	   $(MAKE) PDFLATEX="$(PDFLATEX)" LATEXOPTS="$(LATEXOPTS)" $(DENY_VF) -C $(BUILDDIR)/$(var)/latex || sh $(srctree)/scripts/check-variable-fonts.sh || exit; \
	   mkdir -p $(BUILDDIR)/$(var)/pdf; \
	   mv $(subst .tex,.pdf,$(wildcard $(BUILDDIR)/$(var)/latex/*.tex)) $(BUILDDIR)/$(var)/pdf/; \
	)

endif # HAVE_PDFLATEX

epubdocs:
	@$(srctree)/scripts/sphinx-pre-install --version-check
	@+$(foreach var,$(SPHINXDIRS),$(call loop_cmd,sphinx,epub,$(var),epub,$(var)))

xmldocs:
	@$(srctree)/scripts/sphinx-pre-install --version-check
	@+$(foreach var,$(SPHINXDIRS),$(call loop_cmd,sphinx,xml,$(var),xml,$(var)))

endif # HAVE_SPHINX

# The following targets are independent of HAVE_SPHINX, and the rules should
# work or silently pass without Sphinx.

refcheckdocs:
	$(Q)cd $(srctree);scripts/documentation-file-ref-check

cleandocs:
	$(Q)rm -f $(YNL_INDEX) $(YNL_RST_FILES)
	$(Q)rm -rf $(BUILDDIR)
	$(Q)$(MAKE) BUILDDIR=$(abspath $(BUILDDIR)) $(build)=Documentation/userspace-api/media clean

dochelp:
	@echo  ' Linux kernel internal documentation in different formats from ReST:'
	@echo  '  htmldocs        - HTML'
	@echo  '  texinfodocs     - Texinfo'
	@echo  '  infodocs        - Info'
	@echo  '  latexdocs       - LaTeX'
	@echo  '  pdfdocs         - PDF'
	@echo  '  epubdocs        - EPUB'
	@echo  '  xmldocs         - XML'
	@echo  '  linkcheckdocs   - check for broken external links'
	@echo  '                    (will connect to external hosts)'
	@echo  '  refcheckdocs    - check for references to non-existing files under'
	@echo  '                    Documentation'
	@echo  '  cleandocs       - clean all generated files'
	@echo
	@echo  '  make SPHINXDIRS="s1 s2" [target] Generate only docs of folder s1, s2'
	@echo  '  valid values for SPHINXDIRS are: $(_SPHINXDIRS)'
	@echo
	@echo  '  make SPHINX_CONF={conf-file} [target] use *additional* sphinx-build'
	@echo  '  configuration. This is e.g. useful to build with nit-picking config.'
	@echo
	@echo  '  make DOCS_THEME={sphinx-theme} selects a different Sphinx theme.'
	@echo
	@echo  '  make DOCS_CSS={a .css file} adds a DOCS_CSS override file for html/epub output.'
	@echo
	@echo  '  Default location for the generated documents is Documentation/output'

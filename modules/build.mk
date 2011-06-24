MODULES = \
	  config \
	  echo \
	  help \
	  privileges \
	  snote \
	  userlist \
	  voicebot \
	  whoami \
	  core/channel \
	  core/ctcp \
	  core/die \
	  core/error \
	  core/join_channels \
	  core/mode \
	  core/nickserv \
	  core/oper \
	  core/ping \
	  logs/channel \
	  logs/stderr \
	  privs/account \
	  privs/hostmask \
	  storage/json

storage/json_CXXFLAGS = -Ilibjson

SUBDIRS = $(ENABLE_PERL)

CXXFLAGS = -Isrc -fPIC

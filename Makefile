export VERSION := 0.4.6
export API_VERSION := 7
export WANT_FLAC := 1
export WANT_MP3 := 1
export WANT_WAV := 1

TITLE_ID := 42004E584D555349
MOD_DIR := dist/atmosphere/contents/$(TITLE_ID)
MUSIC_DIR := dist/switch/nx-music/music

.PHONY: all module nxExt app dist clean
all: module app

module: nxExt
	$(MAKE) -C sys-tune

nxExt:
	$(MAKE) -C sys-tune/nxExt

app:
	$(MAKE) -C app

dist: all
	rm -rf dist
	mkdir -p $(MOD_DIR)/flags $(MUSIC_DIR)
	cp sys-tune/sys-tune.nsp $(MOD_DIR)/exefs.nsp
	touch $(MOD_DIR)/flags/boot2.flag.disabled
	cp app/nx-music.nro dist/switch/nx-music/
	touch $(MUSIC_DIR)/.keep
	cp LICENSE dist/NX-Music-LICENSE.txt
	cp README.md dist/NX-Music-README.md

clean:
	$(MAKE) -C sys-tune clean
	$(MAKE) -C sys-tune/nxExt clean
	$(MAKE) -C app clean
	rm -rf dist

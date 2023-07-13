CXX = g++

IDIRS = ../ffmpeg-apple-arm64-build/out/include
LDIRS = ../ffmpeg-apple-arm64-build/out/lib
LIBS_FFMPEG = avcodec avformat avfilter avutil avdevice swscale swresample

OPTS_IDIRS = $(foreach i, $(IDIRS), -I$i)
OPTS_LDIRS = $(foreach l, $(LDIRS), -L$l)
OPTS_LIBS = $(foreach l, $(LIBS_FFMPEG), -l$l)

DEBUGFLAG = -g
CXXFLAGS = $(OPTS_IDIRS) $(OPTS_LDIRS) $(OPTS_LIBS) $(DEBUGFLAG)

mergeaudio: mergeaudio.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

merge: merge.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

crop: crop.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

hello: hello.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

.PHONY: clean
clean:
	rm hello crop merge mergeaudio 2> /dev/null | true
	rm -rf *.dSYM 2> /dev/null | true

# for static compile
# https://ffmpeg.org/pipermail/ffmpeg-user/2015-December/029635.html
# IDIRS = ../ffmpeg-apple-arm64-build/out/include
# LDIRS = ../ffmpeg-apple-arm64-build/out/lib ../ffmpeg-apple-arm64-build/tool/lib
# LIBS_THIRD = x264 x265 vpx vorbis vorbisenc ogg aom fdk-aac opus mp3lame SvtAv1Enc SvtAv1Dec z openh264 iconv bz2 png fribidi freetype harfbuzz ass
# LIBS_FFMPEG = avcodec avformat avfilter avutil # swscale avdevice swresample
# FRAMEWORKS = Security CoreFoundation VideoToolbox AudioToolbox CoreVideo CoreMedia CoreText CoreImage CoreGraphics
#
# OPTS_IDIRS = $(foreach i, $(IDIRS), -I$i)
# OPTS_LDIRS = $(foreach l, $(LDIRS), -L$l)
# OPTS_LIBS = $(foreach l, $(LIBS_FFMPEG) $(LIBS_THIRD), -l$l)
# OPTS_FRAMEWORKS = $(foreach f, $(FRAMEWORKS), -framework $f)
#
# DEBUGFLAG = -g
# CXXFLAGS = $(OPTS_IDIRS) $(OPTS_LDIRS) $(OPTS_LIBS) $(OPTS_FRAMEWORKS) $(DEBUGFLAG)

# libav.so:
# 	$(CC) $(CFLAGS) -shared -o libav.so \
# 		$(foreach l, $(LIBS_FFMPEG), -Wl,-force_load,../ffmpeg-apple-arm64-build/out/lib/lib$l.a)

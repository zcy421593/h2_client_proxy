TARGET = libnghttp2.a

DEPS = nghttp2_pq.o \
	nghttp2_map.o \
	nghttp2_queue.o \
	nghttp2_frame.o \
	nghttp2_buf.o \
	nghttp2_stream.o \
	nghttp2_outbound_item.o \
	nghttp2_session.o \
	nghttp2_submit.o \
	nghttp2_helper.o \
	nghttp2_npn.o \
	nghttp2_hd.o \
	nghttp2_hd_huffman.o \
	nghttp2_hd_huffman_data.o \
	nghttp2_version.o \
	nghttp2_priority_spec.o \
	nghttp2_option.o \
	nghttp2_callbacks.o \
	nghttp2_mem.o \
	nghttp2_http.o

INCLUDES = -I./includes/ \
					 -I./

CFLAGS = -D_U_="" $(INCLUDES)

libnghttp2.a: $(DEPS)
	$(AR) rcs $(TARGET) $(DEPS)

clean:
	rm -f $(DEPS) $(TARGET)
#ifndef _STRING_BUFFER_H_
#define _STRING_BUFFER_H_

/* allocate message buffer */
char* spp_strbuf_allocate(size_t capacity);

/* free message buffer */
void spp_strbuf_free(char* strbuf);

/* append message to buffer */
char* spp_strbuf_append(char *strbuf, const char *append, size_t append_len);

/* remove message from front */
char* spp_strbuf_remove_front(char *strbuf, size_t remove_len);

#endif /* _STRING_BUFFER_H_ */

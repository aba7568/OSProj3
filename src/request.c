#include "io_helper.h"
#include "request.h"
#include <pthread.h>

extern int scheduling_algo;
extern int buffer_max_size;
extern const char *web_root;

//
//	TODO: add code to create and manage the buffer

//synchronization
request *queue;
static int q_front = 0, q_len = 0;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t q_not_full  = PTHREAD_COND_INITIALIZER;

// check if the queue is empty/ful
static int is_empty() {
    return q_len == 0;
}
static int is_full() {
    return q_len == buffer_max_size;
}

//adds request ot block if full
static void enqueue(request req) {
    pthread_mutex_lock(&q_lock);

    //random
    if (scheduling_algo == 2) {
        if (is_full()) {
            int victim = rand() % buffer_max_size;
            queue[victim] = req;
            fprintf(stderr, "[RANDOM] Evicted index %d for new request\n", victim);
        } else {
            int idx = (q_front + q_len) % buffer_max_size;
            queue[idx] = req;
            q_len++;
        }

        pthread_cond_signal(&q_not_empty);
        pthread_mutex_unlock(&q_lock);
        return;
    }
    //waiting
    while (is_full())
        pthread_cond_wait(&q_not_full, &q_lock);

    if (scheduling_algo == 1) {
        //SFF
        queue[q_len] = req;
        q_len++;
        for (int i = q_len - 1; i > 0 && queue[i].filesize < queue[i - 1].filesize; i--) {
            request tmp = queue[i];
            queue[i] = queue[i - 1];
            queue[i - 1] = tmp;
        }
    } else {
	//FIFO
        int idx = (q_front + q_len) % buffer_max_size;
        queue[idx] = req;
        q_len++;
    }

    pthread_cond_signal(&q_not_empty);
    pthread_mutex_unlock(&q_lock);
}

//remove request from queue
static request dequeue() {
    pthread_mutex_lock(&q_lock);
    while (is_empty())
        pthread_cond_wait(&q_not_empty, &q_lock);
    request req;
    if (scheduling_algo == 1) {
	req = queue[0];
	for (int i = 1; i < q_len; i++){
	    queue[i - 1] = queue[i];
        }
    } else {
        req = queue[q_front];
        q_front = (q_front + 1) % buffer_max_size;
    }

    q_len--;
    pthread_cond_signal(&q_not_full);
    pthread_mutex_unlock(&q_lock);
    return req;
}
// Sends out HTTP response in case of errors
//
void request_error(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXBUF], body[MAXBUF];
    
    // Create the body of error message first (have to know its length for header)
    sprintf(body, ""
	    "<!doctype html>\r\n"
	    "<head>\r\n"
	    "  <title>CYB-3053 WebServer Error</title>\r\n"
	    "</head>\r\n"
	    "<body>\r\n"
	    "  <h2>%s: %s</h2>\r\n" 
	    "  <p>%s: %s</p>\r\n"
	    "</body>\r\n"
	    "</html>\r\n", errnum, shortmsg, longmsg, cause);
    
    // Write out the header information for this response
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    write_or_die(fd, buf, strlen(buf));
    
    sprintf(buf, "Content-Type: text/html\r\n");
    write_or_die(fd, buf, strlen(buf));
    
    sprintf(buf, "Content-Length: %lu\r\n\r\n", strlen(body));
    write_or_die(fd, buf, strlen(buf));
    
    // Write out the body last
    write_or_die(fd, body, strlen(body));
    
    // close the socket connection
    close_or_die(fd);
}

//
// Reads and discards everything up to an empty text line
//
void request_read_headers(int fd) {
    char buf[MAXBUF];
    
    readline_or_die(fd, buf, MAXBUF);
    while (strcmp(buf, "\r\n")) {
		readline_or_die(fd, buf, MAXBUF);
    }
    return;
}

//
// Return 1 if static, 0 if dynamic content (executable file)
// Calculates filename (and cgiargs, for dynamic) from uri
//
int request_parse_uri(char *uri, char *filename, char *cgiargs) {
    char *ptr;
    
    if (!strstr(uri, "cgi")) { 
	// static
	strcpy(cgiargs, "");
	sprintf(filename, "%s", uri);
	if (uri[strlen(uri)-1] == '/') {
	    strcat(filename, "index.html");
	}
	return 1;
    } else { 
	// dynamic
	ptr = index(uri, '?');
	if (ptr) {
	    strcpy(cgiargs, ptr+1);
	    *ptr = '\0';
	} else {
	    strcpy(cgiargs, "");
	}
	sprintf(filename, ".%s", uri);
	return 0;
    }
}

//
// Fills in the filetype given the filename
//
void request_get_filetype(char *filename, char *filetype) {
    if (strstr(filename, ".html")) 
		strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif")) 
		strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg")) 
		strcpy(filetype, "image/jpeg");
    else 
		strcpy(filetype, "text/plain");
}

//
// Handles requests for static content
//
#ifndef CLIENT
void request_serve_static(int fd, char *filename, int filesize) {
    int srcfd;
    char *srcp, filetype[MAXBUF], buf[MAXBUF];
    
    request_get_filetype(filename, filetype);
    
    srcfd = open_or_die(filename, O_RDONLY, 0);
    
    // Rather than call read() to read the file into memory, 
    // which would require that we allocate a buffer, we memory-map the file
    srcp = mmap_or_die(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    close_or_die(srcfd);
    
    // put together response
    sprintf(buf, ""
	    "HTTP/1.0 200 OK\r\n"
	    "Server: OSTEP WebServer\r\n"
	    "Content-Length: %d\r\n"
	    "Content-Type: %s\r\n\r\n", 
	    filesize, filetype);
       
    write_or_die(fd, buf, strlen(buf));
    
    //  Writes out to the client socket the memory-mapped file 
    write_or_die(fd, srcp, filesize);
    munmap_or_die(srcp, filesize);
}
#endif

//
// Fetches the requests from the buffer and handles them (thread logic)
//
void* thread_request_serve_static(void* arg) {
	// TODO: write code to actualy respond to HTTP requests
    while (1) {
        pthread_mutex_lock(&q_lock);
        while (is_empty()) {
            pthread_cond_wait(&q_not_empty, &q_lock);
        }

        request req = queue[0];
        for (int i = 1; i < q_len; i++) {
            queue[i - 1] = queue[i];
        }

        q_len--;
	pthread_cond_signal(&q_not_full);
        pthread_mutex_unlock(&q_lock);

        const char* sched_names[] = {"FIFO", "SFF", "RANDOM"};
        printf("serving URI=%s (fd=%d) with %s\n", req.filename, req.fd, sched_names[scheduling_algo]);

        request_serve_static(req.fd, req.filename, req.filesize);
        close_or_die(req.fd);
    }
    return NULL;
}

//
// Initial handling of the request
//
void request_handle(int fd) {
    int is_static;
    struct stat sbuf;
    char buf[MAXBUF], method[MAXBUF], uri[MAXBUF], version[MAXBUF];
    char filename[MAXBUF], cgiargs[MAXBUF];
    
	// get the request type, file path and HTTP version
    readline_or_die(fd, buf, MAXBUF);
    sscanf(buf, "%s %s %s", method, uri, version);
    printf("method:%s uri:%s version:%s\n", method, uri, version);

	// verify if the request type is GET or not
    if (strcasecmp(method, "GET")) {
		request_error(fd, method, "501", "Not Implemented", "server does not implement this method");
		return;
    }
    request_read_headers(fd);
    
	// check requested content type (static/dynamic)
    is_static = request_parse_uri(uri, filename, cgiargs);
    
	// get some data regarding the requested file, also check if requested file is present on server
    char fullpath[MAXBUF];
    snprintf(fullpath, sizeof(fullpath), "%s%s", web_root, filename);

    if (stat(fullpath, &sbuf) < 0) {
		request_error(fd, filename, "404", "Not found", "server could not find this file");
		return;
    }

    
	// verify if requested content is static
    if (is_static) {
		if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
			request_error(fd, filename, "403", "Forbidden", "server could not read this file");
			return;
		}
//		request_serve_static(fd, fullpath, sbuf.st_size);

		// TODO: write code to add HTTP requests in the buffer based on the scheduling policy
		request req;
                req.fd = fd;
		strcpy(req.filename, fullpath);
		req.filesize = sbuf.st_size;
		enqueue(req);


    } else {
		request_error(fd, filename, "501", "Not Implemented", "server does not serve dynamic content request");
    }
}

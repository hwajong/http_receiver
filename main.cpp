#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>

void parse_url(const char* url, char* hostname, int* port, char* path)
{
    // copy the original url to work on
    char nurl[1024];
    strcpy(nurl, url);
    
    // check if the address starts with http://
    // e.g. somehost.com/index.php vs. http://somehost.com/index.php
    int offset = 1;
    char* ppath = nurl;
    if(NULL != strstr(ppath, "http://"))
    {   
        ppath = &(nurl[6]);
        offset += 6;
    }   
    
    // finding the hostname
    char nhostname[1024];
    char* tok_str_ptr = strtok(ppath, "/");
    sprintf(nhostname, "%s", tok_str_ptr);
    
    // check if the hostname also comes with a port no or not
    // e.g. somehost.com:8080/index.php
    if(NULL != strstr(nhostname, ":"))
    {   
        tok_str_ptr = strtok(nhostname, ":");
        sprintf(hostname, "%s", tok_str_ptr);
        tok_str_ptr = strtok(NULL, ":");
    	char portstr[16];
        sprintf(portstr, "%s", tok_str_ptr);
        *port = atoi(portstr);
    } else {
        sprintf(hostname, "%s", nhostname);
    }   

	// the rest of the url gives us the path
    // e.g. /index.php in somehost.com/index.php
    const char* p = &(url[strlen(hostname) + offset]);
    sprintf(path, "%s", p);
    if(strcmp(path, "") == 0)
    {
        sprintf(path, "/");
    }

	//printf("*Host: %s\n", hostname);
	//printf("*Port: %d\n", *port);
	//printf("*Path: %s\n", path);
}

// returun http response code
int get_url_doc(const char* url, char* response_status, char* redirect_url)
{
	//printf("\n");
	//printf("*Target url : %s\n", url);

	// break down the url to know the host and path
	char url_host[256] = {0,};
	char url_path[256] = {0,};
	int port = 80;
	parse_url(url, url_host, &port, url_path);

	// we need to find the ip for the host
	struct hostent *hostent;
	if((hostent = gethostbyname(url_host)) == NULL)
	{
		sprintf(response_status, "gethostbyname() error");
		return -1;
	}

	// create socket
	int rsockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(rsockfd < 0)
	{
		sprintf(response_status, "socket() error");
		return -1;
	}

	struct sockaddr_in host_addr;
	bzero((char*)&host_addr, sizeof(host_addr));
	host_addr.sin_port = htons(port);
	host_addr.sin_family = AF_INET;
	bcopy((char*)hostent->h_addr, (char*)&host_addr.sin_addr.s_addr, hostent->h_length);

	// try connecting to the remote host
	if(connect(rsockfd, (struct sockaddr*)&host_addr, sizeof(struct sockaddr)) < 0)
	{
		sprintf(response_status, "connect() error");
		close(rsockfd);
		return -1;
	}

	//printf("*Connected to IP: %s\n", inet_ntoa(host_addr.sin_addr));

	char buffer[8192] = {0,}; // 8K
	sprintf(buffer,"GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", url_path, url_host);
	// send the request to remote host
	int n = send(rsockfd, buffer, strlen(buffer), 0);
	if(n < 0)
	{
		sprintf(response_status, "send() error");
		close(rsockfd);
		return -1;
	}


	int response_code = 0;
	FILE* fp = NULL;
	int count_recv = 0;
	int content_len = 0;
	bool is_chunked_encoding = false;
	int remaind = 0;
	do
	{
		// recieve from remote host
		bzero((char*)buffer, sizeof(buffer));
		n = recv(rsockfd, buffer, sizeof(buffer), 0);

		//printf("********* nrecv : %d\n", n);

		// if we have read anything - otherwise END-OF-FILE
		if(n > 0)
		{
			char* body = buffer;

			// if this is the first time we are here
			// meaning: we are reading the http response header
			if(count_recv++ == 0)
			{
				// read the first line to discover the response code
				float ver;
				sscanf(buffer, "HTTP/%f %d", &ver, &response_code);
				//printf("*Response Code: %d\n", response_code);

				// save first one line
				const char* p = strstr(buffer, "\r\n");
				strncpy(response_status, buffer, p-buffer);

				if(response_code / 100 == 3) // 3XX
				{
					close(rsockfd);
					// read Location
					p = strstr(buffer, "Location: ");
					if(p != NULL)
					{
						sscanf(p, "Location: %s", redirect_url);
						//printf("*Location: %s\n", redirect_url);
					}

					return response_code;
				}	
				
				if(response_code / 100 != 2) // 2XX
				{
					close(rsockfd);
					return response_code;
				}

				// read Content-Length
				p = strstr(buffer, "Content-Length:");
				if(p!= NULL)
				{
					sscanf(p, "Content-Length: %d", &content_len);
					//printf("*Content-Length: %d\n", content_len);
				}

				// read Connection
				char connection[10] = {0,};
				p = strstr(buffer, "Connection: ");
				if(p != NULL)
				{
					sscanf(p, "Connection: %s", connection);
					//printf("*Connection: %s\n", connection);
				}

				// read Content-Type
				char content_type[300] = {0,};
				char content_type_ext[300] = {0,};
				p = strstr(buffer, "Content-Type: ");
				if(p != NULL)
				{
					sscanf(p, "Content-Type: %s", content_type);
					//printf("*Content-Type: [%s]\n", content_type);
					char* pp = strstr(content_type, "/");
					pp = pp == NULL ? content_type : pp + 1;
					strcpy(content_type_ext, pp);
					int size = strlen(content_type_ext);
					if(content_type_ext[size-1] == ';') content_type_ext[size-1] = 0;
					//printf("*Content-Type ext: [%s]\n", content_type_ext);
				}

				p = strstr(buffer, "Transfer-Encoding: chunked");
				if(p != NULL)
				{
					is_chunked_encoding = true;
					//printf("chunked encoding !!\n");
				}

				// open file for writing
				char fname[256] = {0,};
				sprintf(fname, "./temp/%s.%s", url_host, content_type_ext);
				fp = fopen(fname, "w");

				body = strstr(buffer, "\r\n\r\n") + 4;
			}

			//printf("%s", buffer);
			//printf("%s", body);
			if(content_len == 0)
			{
				if(is_chunked_encoding) // chunked encoding 처리 
				{
					int chunk_size = 0;
					const char* pchunk = body;
					
					// 직전에 다 읽지 못한 나머지 읽어 처리
					if(remaind > 0)
					{
						//printf("remaind : %d\n", remaind);
						fwrite(pchunk, 1, remaind, fp);
						pchunk += remaind;
						remaind = 0;

						int cur_pos = pchunk - buffer;
						if(cur_pos >= n) continue;

						pchunk = strstr(pchunk, "\n") + 1;
					}

					// Loop 돌며 chunk 단위로 처리
					while(true)
					{
						sscanf(pchunk, "%X", &chunk_size);
						//printf("chunk size : %d ( %x )\n", chunk_size, chunk_size);
						if(chunk_size == 0) break;

						pchunk = strstr(pchunk, "\n") + 1;

						int cur_pos = pchunk - buffer;
						//printf("cur pos : %d\n", cur_pos);
						//printf("cur pos + chunk_size : %d\n", cur_pos + chunk_size);
						if(cur_pos + chunk_size > n)
						{
							remaind = chunk_size;
							chunk_size = n - cur_pos;
							remaind -= chunk_size;
							//printf("llll - chunk size : %d ( %x )\n", chunk_size, chunk_size);
							fwrite(pchunk, 1, chunk_size, fp);
							
							break;
						}
						else
						{
							remaind = 0;
							fwrite(pchunk, 1, chunk_size, fp);
							pchunk += chunk_size;
							pchunk = strstr(pchunk, "\n") + 1;

							cur_pos = pchunk - buffer;
							//printf("cur pos : %d\n", cur_pos);
							if(cur_pos >= n) break;
						}
					}
				}
				else
				{
					fprintf(fp, "%s", body);
				}
			}
			else // Content-Length 가 헤더에 있을 경우
			{
				int header_size = body - buffer;
				int body_size = sizeof(buffer) - header_size;
				if(content_len < body_size) body_size = content_len; 

				int nwrite = fwrite(body, 1, body_size, fp);
				//printf("%d - content_len : %d nwrite : %d\n", count_recv, content_len, nwrite);
				content_len -= nwrite;
			}
		}
	} while(n > 0);

	if(fp != NULL) fclose(fp);
	close(rsockfd);

	return response_code;
}

bool user_input_for_fnames(FILE** fp_in, FILE** fp_out)
{
	char fname_in[256] = {0,};
	printf("\nEnter the input file name: ");
	scanf("%s", fname_in);

	*fp_in = fopen(fname_in, "r");
	if(*fp_in == NULL)
	{
		printf("**Error** can't open input file : %s\n\n", fname_in);
		return false;
	}

	char fname_out[256] = {0,};
	printf("\nEnter the output file name: ");	
	scanf("%s", fname_out);

	*fp_out = fopen(fname_out, "w");
	if(*fp_out == NULL)
	{
		printf("**Error** can't open output file : %s\n\n", fname_out);
		return false;
	}

	printf("\n");
	return true;
}

void trim(char* s)
{
	char* p = s;
	int l = strlen(p);

	while(isspace(p[l - 1])) p[--l] = 0;
	while(*p && isspace(*p)) ++p, --l;

	memmove(s, p, l + 1);
}

int main(int argc, char **argv)
{
	FILE* fp_in  = NULL;
	FILE* fp_out = NULL;

	if(!user_input_for_fnames(&fp_in, &fp_out))
	{
		return 0;
	}

	while(true)
	{
		// 한라인씩 읽어 처리
		char line[1024] = {0,};
		if(fgets(line, sizeof(line), fp_in) == NULL)
		{
			break; // end of file
		}

		trim(line);
		if(strlen(line) == 0) continue;

		int no = 0;
		char url[1024] = {0,};
		sscanf(line, "%d. %s", &no, url);

		// printf("%d : %s\n", no, url);
		
		char redirect_url[1024] = {0,};
		char response_status[1024] = {0,};
		int response_code = get_url_doc(url, response_status, redirect_url);
		fprintf(fp_out, "%d. %s\n\n", no, response_status);

		if(response_code / 100 == 3) // 3XX
		{
			fprintf(fp_out, "Location: %s\n\n", redirect_url);
			get_url_doc(redirect_url, response_status, redirect_url);
		}
	}

	fclose(fp_in);
	fclose(fp_out);

	return 0;
}










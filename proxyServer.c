
#include "threadpool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>
#include <ctype.h>

#define MAX_BUFFER_SIZE 1024
#define MAX_REQUESTS 200

typedef struct {
    int client_socket;
    int port_number;
    char *file_data;
    char host[MAX_BUFFER_SIZE];
    struct hostent *host_info;
    // Add other necessary members
} Data;

int checkExactlyIP(char *host, char *data) {

    // Resolve hostname to IP address
    struct hostent *he;
    if ((he = gethostbyname(host)) == NULL) {
        herror("gethostbyname");
        return -1;
    }
    struct in_addr **addr_list = (struct in_addr **) he->h_addr_list;

    // Convert host IP to canonical representation
    char host_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, addr_list[0], host_ip_str, INET_ADDRSTRLEN);

    char *copy = strdup(data); // Make a copy to avoid modifying the original string
    if (copy == NULL) {
        perror("Memory allocation failed");
        return -1;
    }

    char *line = strtok(copy, "\n");

    while (line != NULL) {
        if (!isdigit((line[0]))) {
            // Move to the next line
            line = strtok(NULL, "\r\n");
            continue;
        }

        // Check if the line starts with a digit
        // Check if the line contains an IP address with a mask
        char *slash = strchr(line, '/');
        char *ip_str = line;

        if (slash != NULL) {
            // Extract the IP address and the mask
            *slash = '\0';
        }
        // Compare the host IP with the line's IP
        if (strcmp(host_ip_str, ip_str) == 0) {
            free(copy); // Free memory allocated for the copy
            return 0;
        }

        // Move to the next line
        line = strtok(NULL, "\n");
    }

    free(copy); // Free memory allocated for the copy
    return -1;
}

int searchHostInFile(char *data, char *hostname) {
    char *copy = strdup(data); // Make a copy to avoid modifying the original string
    if (copy == NULL) {
        perror("Memory allocation failed");
        return -1;
    }
    char *line = strtok(copy, "\n");

    // Tokenize the string by newline characters
    while (line != NULL) {
        // Skip lines that start with a number
        if (isdigit(line[0])) {
            line = strtok(NULL, "\n");
            continue;
        }

        if (strstr(line, hostname) != NULL) {
            free(copy); // Free memory allocated for the copy
            return 0;  // If you want to find only the first occurrence, you can remove this line
        }
        line = strtok(NULL, "\n");
    }

    free(copy); // Free memory allocated for the copy
    return -1;
}

int checkSubnet(char *host, char *data) {
    char *copy = strdup(data); // Make a copy to avoid modifying the original string
    if (copy == NULL) {
        perror("Memory allocation failed");
        return -1;
    }
    char *line = strtok(copy, "\n");

    // Iterate through each line in the data string
    while (line != NULL) {
        // Skip lines that start with a letter
        if (!isdigit(line[0])) {
            line = strtok(NULL, "\n");
            continue;
        }

        // Split the line into IP address and mask
        char *slash = strchr(line, '/');
        if (slash == NULL) {
            free(copy);
            return -1;
        }
        *slash = '\0';
        char *ip_str = line;
        int mask = atoi(slash + 1);

        // Resolve hostname to IP address
        struct hostent *he;
        struct in_addr **addr_list;
        if ((he = gethostbyname(host)) == NULL) {
            herror("gethostbyname");
            free(copy);
            return -1;
        }
        addr_list = (struct in_addr **) he->h_addr_list;

        // Convert resolved IP address to binary
        struct in_addr host_ip = *addr_list[0];
        uint32_t host_ip_binary = ntohl(host_ip.s_addr);

        // Convert data IP to binary
        struct in_addr data_ip;
        if (inet_pton(AF_INET, ip_str, &data_ip) != 1) {
            free(copy);
            return -1;
        }
        uint32_t data_ip_binary = ntohl(data_ip.s_addr);

        // Apply mask to both IP addresses
        uint32_t mask_value = (0xFFFFFFFFU << (32 - mask)) & 0xFFFFFFFFU;
        uint32_t masked_host_ip = host_ip_binary & mask_value;
        uint32_t masked_data_ip = data_ip_binary & mask_value;

        // Check if the masked IPs match
        if (masked_host_ip == masked_data_ip) {
            free(copy);
            return 0;
        }

        // Move to the next line
        line = strtok(NULL, "\n");
    }
    free(copy);
    return -1;
}


char *send_error_response(int client_socket, int status_code, const char *status_text, const char *body_text) {
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char date[100];
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", tm);

    char body[1024];
    sprintf(body, "<html><head><title>%d %s</title></head><body><h4>%d %s</h4>%s</body></html>",
            status_code, status_text, status_code, status_text, body_text);

    int content_length = strlen(body);
    char header[1024];
    sprintf(header,
            "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: webserver/1.0\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
            status_code, status_text, date, content_length);

    char *response = malloc(strlen(header) + strlen(body) + 1);
    if (response == NULL) {
        exit(EXIT_FAILURE);  // Consider handling memory allocation failure more gracefully
    }
    sprintf(response, "%s%s", header, body);

    write(client_socket, response, strlen(response));

    free(response);

    return NULL;  // Since we've already sent the response and closed the socket
}

void extractHostFromSocket(char *request, int *flag_for_host, char *host, char *server_port) {
    char *token = strtok(request, " \t\n\r"); // Tokenize input by whitespace characters

    while (token != NULL) {
        if (strcmp(token, "Host:") == 0) { // Check if the token is "Host:"
            *flag_for_host = 0;
            token = strtok(NULL, " \t\n\r"); // Get the next word after "Host:"
            if (token != NULL) {
                char *ptr = strchr(token, ':'); // Find the first occurrence of ':'


                if (ptr != NULL) {
                    // ':' found, so create a new string starting from the character after ':'
                    char *port = strdup(ptr + 1);
                    *ptr = '\0'; // Replace ':' with null character to truncate the original token
                    strncpy(host, token, ptr - token);
                    host[ptr - token] = '\0'; // Null-terminate the string
                    //host[strlen(token) ] = '\0'; // Null-terminate the string

                    strncpy(server_port, port, strlen(port));
                    server_port[strlen(port)] = '\0'; // Null-terminate the string
                    free(port); // Free allocated memory
                    return; // Exit the function after finding and saving the host
                } else {
                    strncpy(host, token, strlen(token));
                    host[strlen(token)] = '\0'; // Null-terminate the string
                    strcpy(server_port, "80");
                }

            }
        }
        token = strtok(NULL, " \t\n\r"); // Move to the next token
    }
}

void handle_client_request(void *arg) {
    Data *data = (Data *) arg;

    char *response = (char *) malloc((MAX_BUFFER_SIZE) * sizeof(char));
    memset(response, 0, MAX_BUFFER_SIZE);

    size_t buffer_size_request = MAX_BUFFER_SIZE;
    char *buffer = (char *) malloc(buffer_size_request * sizeof(char));
    if (buffer == NULL) {
        response = send_error_response(data->client_socket, 500, "Internal Server Error",
                                       "Some server side error.");        // Handle error, send appropriate response to the client
        write(data->client_socket, response, strlen(response));
        free(response);
        return;
    }
    memset(buffer, 0, buffer_size_request);

    // Receive data from client
    int valread;
    char *temp_buffer;
    size_t total_read = 0;
    while ((valread = recv(data->client_socket, buffer + total_read, buffer_size_request - total_read, 0)) > 0) {
        total_read += valread;
        // Check if buffer is full
        if (total_read >= buffer_size_request) {
            // Double the buffer size
            buffer_size_request *= 2;
            temp_buffer = realloc(buffer, buffer_size_request);
            if (temp_buffer == NULL) {
                response = send_error_response(data->client_socket, 500, "Internal Server Error",
                                               "Some server side error.");        // Handle error, send appropriate response to the client
                write(data->client_socket, response, strlen(response));
                free(response);
                free(buffer);
                return;
            }
            buffer = temp_buffer;
        }
        // Check if we received the end of the request
        if (strstr(buffer, "\r\n\r\n") != NULL) {
            break;
        }
    }

    // Check for errors or end of data
    if (valread < 0) {
        response = send_error_response(data->client_socket, 500, "Internal Server Error",
                                       "Some server side error.");        // Handle error, send appropriate response to the client
        write(data->client_socket, response, strlen(response));
        free(response);
        free(buffer);
        return;
    }

    // Process received data


    char *buffer_copy = strdup((buffer));
    char *buffer_for_ip_check = strdup((buffer));
    char *buffer_for_host = strdup((buffer));
    char *buffer_legal_request = strdup((buffer));

    free(buffer);
    if (!buffer_copy || !buffer_for_ip_check || !buffer_for_host || !buffer_legal_request) {
        // Memory allocation failed, handle it appropriately
        if (buffer_copy) free(buffer_copy);
        if (buffer_for_ip_check) free(buffer_for_ip_check);
        if (buffer_for_host) free(buffer_for_host);
        if (buffer_legal_request) free(buffer_legal_request);
        free(response);
        return;
    }


    char body_text[MAX_BUFFER_SIZE];
    char status_text[MAX_BUFFER_SIZE];
    int status_code;

    memset(status_text, 0, MAX_BUFFER_SIZE);
    memset(body_text, 0, MAX_BUFFER_SIZE);




    //start error check

    //***400 bad request***

    //check for 3 tokens
    // Find the first line in the input string
    memset(response, 0, MAX_BUFFER_SIZE);
    const char *firstLine = strtok((char *) buffer_copy, "\n");

    if (firstLine == NULL) {
        // No lines found
        free(response);
        free(buffer_copy);
        free(buffer_for_ip_check);
        free(buffer_for_host);
        free(buffer_legal_request);
        return;
    }

    // Count the number of words in the first line
    int wordCount = 0;
    char firstword[MAX_BUFFER_SIZE];
    char secondword[MAX_BUFFER_SIZE];
    char thirdword[MAX_BUFFER_SIZE];
    char *token = strtok((char *) firstLine, " ");

    while (token != NULL) {
        wordCount++;
        if (wordCount == 1) {
            strcpy(firstword, token);
        }
        if (wordCount == 2) {
            strcpy(secondword, token);
        }
        if (wordCount == 3) {
            strcpy(thirdword, token);
        }
        token = strtok(NULL, " ");
    }

    int flag_for_host = -1; //0 if exist host header
    memset(data->host, 0, MAX_BUFFER_SIZE);
    char server_port[MAX_BUFFER_SIZE];
    memset(server_port, 0, MAX_BUFFER_SIZE);

    extractHostFromSocket(buffer_for_host, &flag_for_host, data->host, server_port);
    // Convert char to int using atoi() function
    data->port_number = atoi(server_port); //80 or something else


    if (strncmp(thirdword, "HTTP/5.6", 8) == 0) {
        //printf("%s\n",thirdword);
        response = send_error_response(data->client_socket, 400, "Bad Request", "Bad Request.");

        // Use the write function to send the response to the client socket
//        write(data->client_socket, response, strlen(response));
        sleep(1);
        close(data->client_socket);
        free(data->file_data);
        free(data);
        free(response);
        free(buffer_copy);
        free(buffer_for_ip_check);
        free(buffer_for_host);
        free(buffer_legal_request);


        return;
    }


    if ((wordCount != 3) || (strlen(secondword)) <= 0 || (strncmp(thirdword, "HTTP/1", 6) != 0) ||
        (flag_for_host != 0)) {
//        status_code = 400;
//        strcpy(status_text, "Bad Request");
//        strcpy(body_text, "Bad Request.");

        response = send_error_response(data->client_socket, 400, "Bad Request", "Bad Request.");

        // Use the write function to send the response to the client socket
        write(data->client_socket, response, strlen(response));
        close(data->client_socket);
        free(data->file_data);
        free(data);
        free(response);
        free(buffer_copy);
        free(buffer_for_ip_check);
        free(buffer_for_host);
        free(buffer_legal_request);
        return;
    }

    //501 error not sported
    //check the GET

    if (firstword != NULL) {
        //printf("%s\n",firstword);
        // Check if the first word is "Get"
        if (firstword[0] != 'G' && firstword[1] != 'E' && firstword[2] != 'T') {
            status_code = 501;
            strcpy(status_text, "Not Supported");
            strcpy(body_text, "Method is not supported.");
            // Call the function to send the response
            response = send_error_response(data->client_socket, status_code, status_text, body_text);
            sleep(1);
            close(data->client_socket);
            free(response);

            free(data->file_data);
            free(data);
            free(buffer_copy);
            free(buffer_for_ip_check);
            free(buffer_for_host);
            free(buffer_legal_request);
            return;
        }
    }


    //404 error not found
    //can't get the IP of the server



    // Call gethostbyname to retrieve information about the host
    data->host_info = gethostbyname(data->host);

    if (data->host_info == NULL) {
        // If gethostbyname fails, print an error message
        status_code = 404;
        strcpy(status_text, "Not Found");
        strcpy(body_text, "File not found.");
        response = send_error_response(data->client_socket, status_code, status_text, body_text);
        // Use the write function to send the response to the client socket
        ssize_t bytes_written = write(data->client_socket, response, 400);
        if (bytes_written == -1) {
            response = send_error_response(data->client_socket, 500, "Internal Server Error",
                                           "Some server side error.");        // Handle error, send appropriate response to the client
//            write(data->client_socket, response, strlen(response));
            close(data->client_socket);
            free(data->file_data);
            free(data);
            free(response);
            free(buffer_copy);
            free(buffer_for_ip_check);
            free(buffer_for_host);
            free(buffer_legal_request);
            return;
        }
        close(data->client_socket);
        free(data->file_data);
        free(data);
        free(response);
        free(buffer_copy);
        free(buffer_for_ip_check);
        free(buffer_for_host);
        free(buffer_legal_request);
        return;
    }


    //403 error forbidden
    //filter check


    if ((checkSubnet(data->host, data->file_data) == 0) || (searchHostInFile(data->file_data, data->host) == 0) ||
        (checkExactlyIP(data->host, data->file_data) == 0))//check if the host apper in the filter file
    {
        status_code = 403;
        strcpy(status_text, "Forbidden");
        strcpy(body_text, "Access denied.");
        send_error_response(data->client_socket, status_code, status_text, body_text);
        // Use the write function to send the response to the client socket
        close(data->client_socket);
        free(data->file_data);
        free(data);
        free(response);
        free(buffer_copy);
        free(buffer_for_ip_check);
        free(buffer_for_host);
        free(buffer_legal_request);
        return;
    }

// *********If the request is valid, forward it to the destination server**********

    //check foe the Conection close in the request header
    char *connection_header = strstr(buffer_legal_request, "Connection:");
    if (connection_header != NULL) {
        // Case 1: "Connection: keep-alive"
        char *keep_alive = strstr(connection_header, "keep-alive");
        if (keep_alive != NULL) {
            // Replace "keep-alive" with "close"
            memcpy(keep_alive, "close", strlen("close"));
            // Remove extra characters ("alive")
            memset(keep_alive + strlen("close"), ' ', strlen("alive"));
        } else {
            // Case 2: "Connection: close" (do nothing)
            // You can add more cases if needed in the future
        }
    } else {
        // Case 3: No "Connection" header in the request
        // Add "Connection: close" to the request header
        char *end_of_request = strstr(buffer_legal_request, "\r\n\r\n");
        if (end_of_request != NULL) {
            // Found the end of the request, add "Connection: close"
            sprintf(end_of_request + 2, "Connection: close\r\n\r\n");
        }
    }

    // Create a socket to connect to the destination server
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        response = send_error_response(data->client_socket, 500, "Internal Server Error",
                                       "Some server side error.");        // Handle error, send appropriate response to the client
        write(data->client_socket, response, strlen(response));
        free(response);
        // Handle error, send appropriate response to the client
        close(data->client_socket);
        free(data->file_data);
        free(data);
        free(buffer_copy);
        free(buffer_for_ip_check);
        free(buffer_for_host);
        free(buffer_legal_request);
        return;
    }

    // Set up the destination server's address
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(struct sockaddr_in));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(data->port_number); // Assuming HTTP, adjust the port if necessary

    //inet_pton(AF_INET, ipAddr, &(server_addr.sin_addr));
    memcpy(&server_addr.sin_addr.s_addr, data->host_info->h_addr, data->host_info->h_length);


    // Connect to the destination server
    if (connect(server_socket, (struct sockaddr *) &server_addr, sizeof(struct sockaddr_in)) < 0) {
        response = send_error_response(data->client_socket, 500, "Internal Server Error",
                                       "Some server side error.");        // Handle error, send appropriate response to the client
        write(data->client_socket, response, strlen(response));
        free(response);
        close(server_socket);
        close(data->client_socket);
        free(data->file_data);
        free(buffer_copy);
        free(buffer_for_ip_check);
        free(buffer_for_host);
        free(buffer_legal_request);
        free(data);
        return;
    }

    // Set a timeout for the recv function
    struct timeval timeout;
    timeout.tv_sec = 10;  // Set the timeout to 10 seconds
    timeout.tv_usec = 0;
    if (setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1) {
        response = send_error_response(data->client_socket, 500, "Internal Server Error",
                                       "Some server side error.");        // Handle error, send appropriate response to the client
        write(data->client_socket, response, strlen(response));
        free(response);
        close(server_socket);
        close(data->client_socket);
        free(data->file_data);
        free(data);
        free(buffer_copy);
        free(buffer_for_ip_check);
        free(buffer_for_host);
        free(buffer_legal_request);
        return;
    }

    // Send the original HTTP request to the destination server
    ssize_t bytes_sent_to_server = send(server_socket, buffer_legal_request, strlen(buffer_legal_request), 0);
    if (bytes_sent_to_server == -1) {
        response = send_error_response(data->client_socket, 500, "Internal Server Error",
                                       "Some server side error.");        // Handle error, send appropriate response to the client
        write(data->client_socket, response, strlen(response));
        free(response);
        close(server_socket);
        close(data->client_socket);
        free(data->file_data);
        free(data);
        free(buffer_copy);
        free(buffer_for_ip_check);
        free(buffer_for_host);
        free(buffer_legal_request);
        return;
    }

    size_t response_buffer_size = MAX_BUFFER_SIZE;  // Initial buffer size
    char *response_buffer_from_server = (char *) malloc(response_buffer_size * sizeof(char));
    memset(response_buffer_from_server, 0, response_buffer_size);

    if (response_buffer_from_server == NULL) {
        response = send_error_response(data->client_socket, 500, "Internal Server Error",
                                       "Some server side error.");        // Handle error, send appropriate response to the client
        write(data->client_socket, response, strlen(response));
        free(response);
        close(server_socket);
        close(data->client_socket);
        free(data->file_data);
        free(data);
        free(buffer_copy);
        free(buffer_for_ip_check);
        free(buffer_for_host);
        free(buffer_legal_request);
        return;
    }

    // Receive data from server
    int val_read_from_server;
    char *temp_buffer_for_server_response;
    size_t total_read_from_server = 0;
    while ((val_read_from_server = recv(server_socket, response_buffer_from_server + total_read_from_server,
                                        response_buffer_size - total_read_from_server, 0)) > 0) {
        total_read_from_server += val_read_from_server;
        // Check if buffer is full
        if (total_read_from_server >= response_buffer_size) {
            // Double the buffer size
            response_buffer_size *= 2;
            temp_buffer_for_server_response = realloc(response_buffer_from_server, response_buffer_size);
            if (temp_buffer_for_server_response == NULL) {
                response = send_error_response(data->client_socket, 500, "Internal Server Error",
                                               "Some server side error.");        // Handle error, send appropriate response to the client
                write(data->client_socket, response, strlen(response));
                free(response);
                free(response_buffer_from_server);
                close(data->client_socket);
                free(data->file_data);
                free(data);
                free(buffer_copy);
                free(buffer_for_ip_check);
                free(buffer_for_host);
                free(buffer_legal_request);
                return;
            }
            response_buffer_from_server = temp_buffer_for_server_response;
        }

    }


    // Check for errors or end of data
    if (val_read_from_server < 0) {
        response = send_error_response(data->client_socket, 500, "Internal Server Error",
                                       "Some server side error.");        // Handle error, send appropriate response to the client
        write(data->client_socket, response, strlen(response));
        free(response);
        free(response_buffer_from_server);
        close(data->client_socket);
        free(data->file_data);
        free(data);
        free(buffer_copy);
        free(buffer_for_ip_check);
        free(buffer_for_host);
        free(buffer_legal_request);
        return;
    }

    // Process received data

    // Send the response back to the original client
    size_t bytes_sent_back_to_clinte = write(data->client_socket, response_buffer_from_server, total_read_from_server);
    if (bytes_sent_back_to_clinte == -1) {
        response = send_error_response(data->client_socket, 500, "Internal Server Error",
                                       "Some server side error.");        // Handle error, send appropriate response to the client
        write(data->client_socket, response, strlen(response));
        free(response);
        free(response_buffer_from_server);
        close(data->client_socket);
        free(data->file_data);
        free(data);
        free(buffer_copy);
        free(buffer_for_ip_check);
        free(buffer_for_host);
        free(buffer_legal_request);
        close(server_socket);
        return;
    }

    // Clean up
    free(buffer_copy);
    free(buffer_for_ip_check);
    close(data->client_socket);
    free(data->file_data);
    free(data);
    free(buffer_for_host);
    free(buffer_legal_request);
    free(response);
    free(response_buffer_from_server);
}

int main(int argc, char *argv[]) {

    int port = atoi(argv[1]);
    // Parse command line arguments

    int max_requests = atoi(argv[3]);
    // Check command line arguments
    if ((argc != 5) || (port < 1024) || (port >= 65536) || (max_requests > MAX_REQUESTS)) {
        fprintf(stderr, "Usage: proxyServer <port> <pool-size> <max-number-of-request> <filter>");
        exit(EXIT_FAILURE);
    }
    int pool_size = atoi(argv[2]);
    //check the filter path
    FILE *file;
    char line[MAX_BUFFER_SIZE];

    const char *filter_path = argv[4];
    size_t file_data_size = 0;

    // Open the file for reading
    file = fopen(filter_path, "r");
    if (file == NULL) {
        fprintf(stderr, "Usage: %s <port> <pool-size> <max-number-of-request> <filter>\n", argv[0]);
        exit(EXIT_FAILURE);
    }


    char *temp_file_data = (char *) malloc(MAX_BUFFER_SIZE * sizeof(char)); //TODO update struct with it
    memset(temp_file_data, 0, MAX_BUFFER_SIZE);

    // Read lines from the file
    while (fgets(line, sizeof(line), file) != NULL) {
        // Append the line to file_data
        size_t line_length = strlen(line);
        char *temp = realloc(temp_file_data, file_data_size + line_length + 2); // 2 for '\n' and '\0'
        if (temp == NULL) {
            perror("error: realloc\n");
            free(temp_file_data);
            fclose(file);
            return EXIT_FAILURE;
        }
        temp_file_data = temp;
        strcat(temp_file_data, line);
        strcat(temp_file_data, "\n");
        file_data_size += line_length + 2; // Increase the size
    }

    // Close the file
    fclose(file);

    // Output the file data


    // Initialize the thread pool
    threadpool *pool = create_threadpool(pool_size); //prob
    if (pool == NULL) {
        fprintf(stderr, "Failed to create thread pool\n");
        exit(EXIT_FAILURE);
    }

    // Set up server socket
    int w_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (w_socket == -1) {
        perror("error: socket\n");
        destroy_threadpool(pool);
        exit(EXIT_FAILURE);
    }

    // Set up server address struct
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);


    // Bind the server socket to the specified port
    // TODO handel perror- put the correct error
    if (bind(w_socket, (struct sockaddr *) &server_addr, sizeof(server_addr)) == -1) {
        perror("error: bind\n");//TODO change string
        close(w_socket);
        destroy_threadpool(pool); //TODO yes?
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    //TODO handel perror- put the correct error
    if (listen(w_socket, SOMAXCONN) == -1) {
        perror("error: listen\n");
        close(w_socket);
        destroy_threadpool(pool); //TODO yes?
        exit(EXIT_FAILURE);
    }



    // Main server loop
    //TODO handel perror- put the correct error
    for (int i = 0; i < max_requests;) {
        Data *data = malloc(sizeof(Data)); //prob
        if (data == NULL) {
            perror("error: malloc\n");
            destroy_threadpool(pool);
            exit(EXIT_FAILURE);
        }
        // Accept a new connection
        data->client_socket = accept(w_socket, NULL, NULL);
        if (data->client_socket == -1) {
            perror("error: accept\n");
            break;
        }
        data->file_data = strdup(temp_file_data);



        // Enqueue the client socket into the thread pool for processing
        dispatch(pool, (dispatch_fn) handle_client_request, (void *) data);
        sleep(3);   //to let the threads finish

        i++;
    }
    free(temp_file_data);
    destroy_threadpool(pool);
    close(w_socket);
    exit(0);
}





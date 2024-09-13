/*****************************************************************************/
/*                       CSC209-24s A4 Audio Stream                          */
/*       Copyright 2024 -- Demetres Kostas PhD (aka Darlene Heliokinde)      */
/*****************************************************************************/
#include "as_client.h"
#include <time.h>

/* HELPERS */

/**
 * @brief Helper for send_and_process_stream_request(), gets
 * the max of the 3 file descriptors used for select() call
 * 
 * @param fd1 
 * @param fd2 
 * @param fd3 
 * @return int: the maximum file descriptor
 */
int _get_max_fd(int fd1, int fd2, int fd3){
    if (fd1 >= fd2 && fd1 >= fd3){
        return fd1;
    }
    else if (fd2 >= fd1 && fd2 >= fd3){
        return fd2;
    }
    else{
        return fd3;
    }
}

/**
 * @brief Helper for send_and_process_stream_request(), closes the audio and file
 * fds at once when needed.
 * 
 * @param fd1 
 * @param fd2 
 * @return int 
 */
int _close_all_fds(int fd1, int fd2){
    if (fd1 >= 0){
        if (close(fd1) < 0){
            perror("close");
            return -1;
        }
    }

    if (fd2 >= 0){
        if (close(fd2) < 0){
            perror("close");
            return -1;
        }
    }

    return 0;
}

/* OFFICIAL ASSIGNMENT FUNCTIONS */

static int connect_to_server(int port, const char *hostname) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("connect_to_server");
        return -1;
    }

    struct sockaddr_in addr;

    // Allow sockets across machines.
    addr.sin_family = AF_INET;
    // The port the server will be listening on.
    // htons() converts the port number to network byte order.
    // This is the same as the byte order of the big-endian architecture.
    addr.sin_port = htons(port);
    // Clear this field; sin_zero is used for padding for the struct.
    memset(&(addr.sin_zero), 0, 8);

    // Lookup host IP address.
    struct hostent *hp = gethostbyname(hostname);
    if (hp == NULL) {
        ERR_PRINT("Unknown host: %s\n", hostname);
        return -1;
    }

    addr.sin_addr = *((struct in_addr *) hp->h_addr);

    // Request connection to server.
    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        return -1;
    }

    return sockfd;
}


/*
** Helper for: list_request
** This function reads from the socket until it finds a network newline.
** This is processed as a list response for a single library file,
** of the form:
**                   <index>:<filename>\r\n
**
** returns index on success, -1 on error
** filename is a heap allocated string pointing to the parsed filename
*/
static int get_next_filename(int sockfd, char **filename) {
    static int bytes_in_buffer = 0;
    static char buf[RESPONSE_BUFFER_SIZE];

    while((*filename = find_network_newline(buf, &bytes_in_buffer)) == NULL) {
        int num = read(sockfd, buf + bytes_in_buffer,
                       RESPONSE_BUFFER_SIZE - bytes_in_buffer);
        if (num < 0) {
            perror("list_request");
            return -1;
        }
        bytes_in_buffer += num;
        if (bytes_in_buffer == RESPONSE_BUFFER_SIZE) {
            ERR_PRINT("Response buffer filled without finding file\n");
            ERR_PRINT("Bleeding data, this shouldn't happen, but not giving up\n");
            memmove(buf, buf + BUFFER_BLEED_OFF, RESPONSE_BUFFER_SIZE - BUFFER_BLEED_OFF);
        }
    }

    char *parse_ptr = strtok(*filename, ":");
    int index = strtol(parse_ptr, NULL, 10);
    parse_ptr = strtok(NULL, ":");
    // moves the filename to the start of the string (overwriting the index)
    memmove(*filename, parse_ptr, strlen(parse_ptr) + 1);

    return index;
}


int list_request(int sockfd, Library *library) {

    /* 1.) Make the request to stream. */
    write_precisely(sockfd, "LIST\r\n", sizeof(char) * 6);

    /* 2.) Set up initial library allocations */
    char **first_file = malloc(sizeof(char *));
    if (first_file == NULL){
        perror("malloc");
        return -1;
    }
    library->num_files = 1;

    // if we have prexisting files, clear the library and free
    if (library->files != NULL){
        _free_library(library);
    }

    /* 3.) Get first file to see how many subsequent read calls are needed */
    int first_index = get_next_filename(sockfd, first_file);
    if (first_index < 0){
        free(first_file);
        perror("get_next_filename");
        return -1;
    }

    /* 4.) Fill the rest of the library */

    // Allocate enough space for all of the files, since we've recovered how many there are
    library->num_files = first_index + 1;
    library->files = malloc(sizeof(char *) * (library->num_files));
    if (library->files == NULL){
        free(first_file);
        perror("malloc");
        return -1;
    }
    library->files[first_index] = *first_file;
    #ifdef DEBUG
    printf("we first loaded '%s' @ index %d\n", library->files[first_index], first_index);
    #endif
    free(first_file);

   
    // Populate the rest of the list
    for (int i = first_index - 1; i >= 0; i--){  
        if (get_next_filename(sockfd, library->files + i) < 0){
            perror("get_next_filename");
            free(library->files);
            return -1;
        }
    }

    /* 5.) If we've reached this point, we've successfully loaded all of the files and can print them. */
    for (int i = 0; i < library->num_files; i++){
        printf("%d: %s\n", i, (library->files)[i]);
    }
    
    return library->num_files;
}

/*
** Get the permission of the library directory. If the library 
** directory does not exist, this function shall create it.
**
** library_dir: the path of the directory storing the audio files
** perpt:       an output parameter for storing the permission of the 
**              library directory.
**
** returns 0 on success, -1 on error
*/
static int get_library_dir_permission(const char *library_dir, mode_t * perpt) {

    struct stat dir_info;

    // If something is wrong with stat call...
    if (stat(library_dir, &dir_info) < 0){

        // ...Either the dir doesn't exist or...
        if (errno == ENOENT) {
            mode_t mode = 0700;
            if (mkdir(library_dir, mode) < 0){
                perror("mkdir");
                return -1;
            }

            if (chmod(library_dir, mode) < 0){
                perror("chmod");
                return -1;
            }

            if (stat(library_dir, &dir_info) < 0){
                perror("stat");
                return -1;
            }
        // ...stat call just failed
        } else {
            perror("stat");
            return -1;
        }
    }
    
    *perpt = dir_info.st_mode;
    
    return 0;
}

/*
** Creates any directories needed within the library dir so that the file can be
** written to the correct destination. All directories will inherit the permissions
** of the library_dir.
**
** This function is recursive, and will create all directories needed to reach the
** file in destination.
**
** Destination shall be a path without a leading /
**
** library_dir can be an absolute or relative path, and can optionally end with a '/'
**
*/
static void create_missing_directories(const char *destination, const char *library_dir) {

    // get the permissions of the library dir
    mode_t permissions;
    if (get_library_dir_permission(library_dir, &permissions) == -1) {
        exit(1);
    }
    
    char *str_de_tokville = strdup(destination);
    if (str_de_tokville == NULL) {
        perror("create_missing_directories");
        return;
    }

    char *before_filename = strrchr(str_de_tokville, '/');
    if (!before_filename){
        goto free_tokville;
    }

    char *path = malloc(strlen(library_dir) + strlen(destination) + 2);
    if (path == NULL) {
        goto free_tokville;
    } *path = '\0';

    char *dir = strtok(str_de_tokville, "/");
    if (dir == NULL){
        goto free_path;
    }
    strcpy(path, library_dir);
    if (path[strlen(path) - 1] != '/') {
        strcat(path, "/");
    }
    strcat(path, dir);

    while (dir != NULL && dir != before_filename + 1) {
        #ifdef DEBUG
        printf("Creating directory %s\n", path);
        #endif
        if (mkdir(path, permissions) == -1) {
            if (errno != EEXIST) {
                perror("create_missing_directories");
                goto free_path;
            }
        }
        dir = strtok(NULL, "/");
        if (dir != NULL) {
            strcat(path, "/");
            strcat(path, dir);
        }
    }
free_path:
    free(path);
free_tokville:
    free(str_de_tokville);
}


/*
** Helper for: get_file_request
*/
static int file_index_to_fd(uint32_t file_index, const Library * library){
    create_missing_directories(library->files[file_index], library->path);

    char *filepath = _join_path(library->path, library->files[file_index]);
    if (filepath == NULL) {
        return -1;
    }

    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    #ifdef DEBUG
    printf("Opened file %s\n", filepath);
    #endif
    free(filepath);
    if (fd < 0 ) {
        perror("file_index_to_fd");
        return -1;
    }

    return fd;
}


int get_file_request(int sockfd, uint32_t file_index, const Library * library){
    #ifdef DEBUG
    printf("Getting file %s\n", library->files[file_index]);
    #endif

    int file_dest_fd = file_index_to_fd(file_index, library);
    if (file_dest_fd == -1) {
        return -1;
    }

    int result = send_and_process_stream_request(sockfd, file_index, -1, file_dest_fd);
    if (result == -1) {
        return -1;
    }

    return 0;
}


int start_audio_player_process(int *audio_out_fd) {

    // 1.) Create the pipe architecture
    int pipe_fd[2];
    if (pipe(pipe_fd) < 0){
        perror("pipe");
        return -1;
    }
    int pipe_read = pipe_fd[0];
    int pipe_write = pipe_fd[1];

    // 2.) Create a child process for running mpv
    int pid = fork();

    if (pid < 0){
        close(pipe_read);
        close(pipe_write);
        perror("fork");
        return -1;
    }

    else if (pid == 0) {
        // We're in child process, call exec
        if (close(pipe_write) < 0){
            perror("close");
            return -1;
        }

        // make the read end of our pipe the stdin of our new process
        if (dup2(pipe_read, STDIN_FILENO) < 0){
            perror("dup2");
            close(pipe_read);
            return -1;
        } 

        if (close(pipe_read) < 0){
            perror("close");
            return -1;
        }

        char *args[] = AUDIO_PLAYER_ARGS;
        execvp(AUDIO_PLAYER, args);

        // If we get here, an error has occured
        perror("execvp");
        return -1;
    }

    else{ // we're in parent process
        if (close(pipe_read) < 0){
            perror("close");
            return -1;
        }
        
        // wait for audio player to boot before continuing
        sleep(AUDIO_PLAYER_BOOT_DELAY); 

        *audio_out_fd = pipe_write;

        return pid;
    }
}


static void _wait_on_audio_player(int audio_player_pid) {
    int status;
    if (waitpid(audio_player_pid, &status, 0) == -1) {
        perror("_wait_on_audio_player");
        return;
    }
    if (WIFEXITED(status)) {
        fprintf(stderr, "Audio player exited with status %d\n", WEXITSTATUS(status));
    } else {
        printf("Audio player exited abnormally\n");
    }
}


int stream_request(int sockfd, uint32_t file_index) {
    int audio_out_fd;
    int audio_player_pid = start_audio_player_process(&audio_out_fd);

    int result = send_and_process_stream_request(sockfd, file_index, audio_out_fd, -1);
    if (result == -1) {
        ERR_PRINT("stream_request: send_and_process_stream_request failed\n");
        return -1;
    }

    _wait_on_audio_player(audio_player_pid);

    return 0;
}


int stream_and_get_request(int sockfd, uint32_t file_index, const Library * library) {
    int audio_out_fd;
    int audio_player_pid = start_audio_player_process(&audio_out_fd);

    #ifdef DEBUG
    printf("Getting file %s\n", library->files[file_index]);
    #endif

    int file_dest_fd = file_index_to_fd(file_index, library);
    if (file_dest_fd == -1) {
        ERR_PRINT("stream_and_get_request: file_index_to_fd failed\n");
        return -1;
    }

    int result = send_and_process_stream_request(sockfd, file_index,
                                                 audio_out_fd, file_dest_fd);
    if (result == -1) {
        ERR_PRINT("stream_and_get_request: send_and_process_stream_request failed\n");
        return -1;
    }

    _wait_on_audio_player(audio_player_pid);

    return 0;
}


int send_and_process_stream_request(int sockfd, uint32_t file_index,
                                    int audio_out_fd, int file_dest_fd) {

    // 1.) Send the request to stream
    uint32_t nw_file_index = htonl(file_index);
    if (write_precisely(sockfd, "STREAM\r\n", 8) < 0 || write_precisely(sockfd, &nw_file_index, sizeof(uint32_t)) < 0){
        perror("write_precisely");
        return -1;
    }
    
    // 2.) Get size of incoming file
    uint32_t file_size = 0;
    if (read_precisely(sockfd, &file_size, sizeof(uint32_t)) < 0){
        perror("read_precisely");
        return -1;
    }
    file_size = ntohl(file_size);

    #ifdef DEBUG
        printf("we got a file size of %d\n", file_size);
    #endif

    // 3.) Set up buffers
    char *static_buf = malloc(NETWORK_PRE_DYNAMIC_BUFF_SIZE);
    if (static_buf == NULL){
        perror("malloc");
        return -1;
    }
    char *dynamic_buf = NULL; 

    // 3.) Reading and writing

    int bytes_left_to_write = (int) file_size; 
    int total_bytes_written_file = 0;
    int total_bytes_written_audio = 0;
    int num_bytes_in_db = 0; // items in dynamic buffer
    int audio_fd_offset = 0; // where in the dynamic buffer has audio_fd been written succesfully? dynamic_buf + offset 
    int file_fd_offset = 0; // where in the dynamic buffer has file_fd been written succesfully? dynamic_buf + offset 

    while (bytes_left_to_write > 0 && (audio_out_fd >= 0 || file_dest_fd >= 0)){
        // a) initialize the fd sets
        fd_set read_fds;
        fd_set write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_SET(sockfd, &read_fds);
        
        // We only add them to our set if we need to write to them in the first place
        // This way, when we check if they're ready, they will never even show up.
        if (audio_out_fd >= 0){
            FD_SET(audio_out_fd, &write_fds);
        }
        if (file_dest_fd >= 0){
            FD_SET(file_dest_fd, &write_fds);
        }

        #ifdef DEBUG
        if (audio_out_fd < 0 && file_dest_fd < 0){
            printf("both fds are bad\n");
            exit(-1);
        }
        #endif
        
        // b) work with fds that are ready
        struct timeval timeout;
        timeout.tv_sec = SELECT_TIMEOUT_SEC;
        timeout.tv_usec = SELECT_TIMEOUT_USEC;
        if (select(_get_max_fd(sockfd, audio_out_fd, file_dest_fd) + 1,
            &read_fds, &write_fds, NULL, &timeout) < 0){
            free(static_buf);
            free(dynamic_buf);
            perror("select");
            return -1;
        }

        // c.) read items from server write them to dynamic buffer if applicable
        if (sockfd >= 0 && FD_ISSET(sockfd, &read_fds)){
            int num_bytes_read;
            if ((num_bytes_read = read(sockfd, static_buf, NETWORK_PRE_DYNAMIC_BUFF_SIZE)) < 0){
                free(static_buf);
                free(dynamic_buf);
                perror("read");
                return -1;
            }

            #ifdef DEBUG
            printf("Succesfully read something \n");
            #endif

            dynamic_buf = realloc(dynamic_buf, num_bytes_in_db + num_bytes_read);
            if (dynamic_buf == NULL){
                free(static_buf);
                perror("realloc");
                return -1;
            }
            memcpy(dynamic_buf + num_bytes_in_db, static_buf, num_bytes_read);
            num_bytes_in_db += num_bytes_read;
        }

        // d) if there are no items in dynamic buffer to write, we skip
        if (num_bytes_in_db == 0){
            continue;
        }
        
        // e.) write items from dynamic buffer to audio out if ready
        int num_bytes_written_audio = 0;
        if (audio_out_fd >= 0 && FD_ISSET(audio_out_fd, &write_fds)){
            if ((num_bytes_written_audio = write(audio_out_fd, dynamic_buf + audio_fd_offset, num_bytes_in_db - audio_fd_offset)) < 0){
                #ifdef DEBUG
                printf("audio write failed\n");
                #endif
                free(static_buf);
                free(dynamic_buf);
                perror("write");
                return -1;
            }
        }

        // f.) write items from dynamic buffer to file out if ready
        int num_bytes_written_file = 0;
        if (file_dest_fd >= 0 && FD_ISSET(file_dest_fd, &write_fds)){
            if ((num_bytes_written_file = write(file_dest_fd, dynamic_buf + file_fd_offset, num_bytes_in_db - file_fd_offset)) < 0){
                #ifdef DEBUG
                printf("file write failed @ offset %d and dyn buf size %d\n", file_fd_offset, num_bytes_in_db);
                #endif
                free(static_buf);
                free(dynamic_buf);
                perror("write");
                return -1;
            }
        }

        total_bytes_written_file += num_bytes_written_file;
        total_bytes_written_audio += num_bytes_written_audio;

        // Check if we've written enough to audio and file, then we know to close it
        if (total_bytes_written_audio >= (int) file_size && total_bytes_written_file >= (int) file_size){
            if (_close_all_fds(audio_out_fd, file_dest_fd) < 0){
                return -1;
            }

            audio_out_fd = -1;
            file_dest_fd = -1;
            bytes_left_to_write = 0;
            break;
        }

        // e.) Make adjustments on buffer size based on offsets

        // both destinations are defined and bytes written to file is ahead
        if (total_bytes_written_file > total_bytes_written_audio && audio_out_fd >= 0 && file_dest_fd >= 0){
            #ifdef DEBUG
            printf("file is ahead\n");
            #endif
            int new_buff_size = num_bytes_in_db - num_bytes_written_audio;
            audio_fd_offset = 0;
            
            file_fd_offset = total_bytes_written_file - total_bytes_written_audio;
            #ifdef DEBUG
            printf("num bytes in dynamic buffer %d\n", num_bytes_in_db);
            #endif

            // Check if we've written enough to file, then we know to close it
            if (total_bytes_written_file >= (int) file_size){
                if (close(file_dest_fd) < 0){
                    perror("close");
                    return -1;
                }
                file_dest_fd = -1;
            }

            char *new_dyn_buff = malloc(new_buff_size);
            if (new_dyn_buff == NULL){
                free(static_buf);
                free(dynamic_buf);
                perror("malloc");
                return -1;
            }
            memcpy(new_dyn_buff, dynamic_buf + num_bytes_written_audio, new_buff_size);
            free(dynamic_buf);
            dynamic_buf = new_dyn_buff;
            num_bytes_in_db = new_buff_size;
            bytes_left_to_write -= num_bytes_written_audio;

            #ifdef DEBUG
            printf("num bytes written to file is: %d\n", num_bytes_written_file);
            printf("num bytes written to audio is: %d\n", num_bytes_written_audio);
            printf("num bytes left to write is %d\n", bytes_left_to_write);
            #endif
        }
        // both destinations are defined, and num_bytes_written_audio is larger 
        else if (total_bytes_written_file < total_bytes_written_audio && audio_out_fd >= 0 && file_dest_fd >= 0){ 
            #ifdef DEBUG
            printf("audio is ahead\n");
            #endif
            int new_buff_size = num_bytes_in_db - num_bytes_written_file;
            file_fd_offset = 0;
            
            audio_fd_offset = total_bytes_written_audio - total_bytes_written_file;

            // Check if we've written enough to audio, then we know to close it
            if (total_bytes_written_audio >= (int) file_size){
                if (close(audio_out_fd) < 0){
                    perror("close");
                    return -1;
                }
                audio_out_fd = -1;
            }

            char *new_dyn_buff = malloc(new_buff_size);
            if (new_dyn_buff == NULL){
                free(static_buf);
                free(dynamic_buf);
                perror("malloc");
                return -1;
            }
            memcpy(new_dyn_buff, dynamic_buf + num_bytes_written_file, new_buff_size);
            free(dynamic_buf);
            dynamic_buf = new_dyn_buff;
            num_bytes_in_db = new_buff_size;
            bytes_left_to_write -= num_bytes_written_file; 
            
            #ifdef DEBUG
            printf("num bytes written to file is: %d\n", num_bytes_written_file);
            printf("num bytes written to audio is: %d\n", num_bytes_written_audio);
            printf("num bytes left to write is %d\n", bytes_left_to_write);
            #endif
        }
        // if we've written the same amount total, we can clear the dynamic buffer
        else if (audio_out_fd >= 0 && file_dest_fd >= 0){ 
            #ifdef DEBUG
            printf("both are same\n");
            #endif
            file_fd_offset = 0;
            audio_fd_offset = 0;
            
            free(dynamic_buf);
            dynamic_buf = NULL;
            num_bytes_in_db = 0;
            bytes_left_to_write -= num_bytes_written_file;
            
            #ifdef DEBUG
            printf("num bytes written to file is: %d\n", num_bytes_written_file);
            printf("num bytes written to audio is: %d\n", num_bytes_written_audio);
            printf("num bytes left to write is %d\n", bytes_left_to_write);
            #endif
        }
        // only audio_out_fd is defined
        else if (audio_out_fd >= 0) {
            int new_buff_size = num_bytes_in_db - num_bytes_written_audio;
            audio_fd_offset = 0;

            char *new_dyn_buff = malloc(new_buff_size);
            if (new_dyn_buff == NULL){
                free(static_buf);
                free(dynamic_buf);
                perror("malloc");
                return -1;
            }
            memcpy(new_dyn_buff, dynamic_buf + num_bytes_written_audio, new_buff_size);
            free(dynamic_buf);
            dynamic_buf = new_dyn_buff;
            num_bytes_in_db = new_buff_size;
            bytes_left_to_write -= num_bytes_written_audio; 

            #ifdef DEBUG
            printf("num bytes left to write is %d\n ()", bytes_left_to_write);
            #endif

        }
        // only file fd is defined
        else{
            int new_buff_size = num_bytes_in_db - num_bytes_written_file;
            audio_fd_offset = 0;

            char *new_dyn_buff = malloc(new_buff_size);
            if (new_dyn_buff == NULL){
                free(static_buf);
                free(dynamic_buf);
                perror("malloc");
                return -1;
            }
            memcpy(new_dyn_buff, dynamic_buf + num_bytes_written_file, new_buff_size);
            free(dynamic_buf);
            dynamic_buf = new_dyn_buff;
            num_bytes_in_db = new_buff_size;
            bytes_left_to_write -= num_bytes_written_file; // problem

            #ifdef DEBUG
            printf("num bytes left to write is %d\n ()", bytes_left_to_write);
            #endif
        }
    }
    
    // 4.) close the file descriptors once we're done
    if (_close_all_fds(audio_out_fd, file_dest_fd) < 0){
        return -1;
    }

    free(static_buf);
    free(dynamic_buf);

    return 0;
}


static void _print_shell_help(){
    printf("Commands:\n");
    printf("  list: List the files in the library\n");
    printf("  get <file_index>: Get a file from the library\n");
    printf("  stream <file_index>: Stream a file from the library (without saving it)\n");
    printf("  stream+ <file_index>: Stream a file from the library\n");
    printf("                        and save it to the local library\n");
    printf("  help: Display this help message\n");
    printf("  quit: Quit the client\n");
}


/*
** Shell to handle the client options
** ----------------------------------
** This function is a mini shell to handle the client options. It prompts the
** user for a command and then calls the appropriate function to handle the
** command. The user can enter the following commands:
** - "list" to list the files in the library
** - "get <file_index>" to get a file from the library
** - "stream <file_index>" to stream a file from the library (without saving it)
** - "stream+ <file_index>" to stream a file from the library and save it to the local library
** - "help" to display the help message
** - "quit" to quit the client
*/
static int client_shell(int sockfd, const char *library_directory) {
    char buffer[REQUEST_BUFFER_SIZE];
    char *command;
    int file_index;

    Library library = {"client", library_directory, NULL, 0};

    while (1) {
        if (library.files == 0) {
            printf("Server library is empty or not retrieved yet\n");
        }

        printf("Enter a command: ");
        if (fgets(buffer, REQUEST_BUFFER_SIZE, stdin) == NULL) {
            perror("client_shell");
            goto error;
        }

        command = strtok(buffer, " \n");
        if (command == NULL) {
            continue;
        }

        // List Request -- list the files in the library
        if (strcmp(command, CMD_LIST) == 0) {
            if (list_request(sockfd, &library) == -1) {
                goto error;
            }


        // Get Request -- get a file from the library
        } else if (strcmp(command, CMD_GET) == 0) {
            char *file_index_str = strtok(NULL, " \n");
            if (file_index_str == NULL) {
                printf("Usage: get <file_index>\n");
                continue;
            }
            file_index = strtol(file_index_str, NULL, 10);
            if (file_index < 0 || file_index >= library.num_files) {
                printf("Invalid file index\n");
                continue;
            }

            if (get_file_request(sockfd, file_index, &library) == -1) {
                goto error;
            }

        // Stream Request -- stream a file from the library (without saving it)
        } else if (strcmp(command, CMD_STREAM) == 0) {
            char *file_index_str = strtok(NULL, " \n");
            if (file_index_str == NULL) {
                printf("Usage: stream <file_index>\n");
                continue;
            }
            file_index = strtol(file_index_str, NULL, 10);
            if (file_index < 0 || file_index >= library.num_files) {
                printf("Invalid file index\n");
                continue;
            }

            if (stream_request(sockfd, file_index) == -1) {
                goto error;
            }

        // Stream and Get Request -- stream a file from the library and save it to the local library
        } else if (strcmp(command, CMD_STREAM_AND_GET) == 0) {
            char *file_index_str = strtok(NULL, " \n");
            if (file_index_str == NULL) {
                printf("Usage: stream+ <file_index>\n");
                continue;
            }
            file_index = strtol(file_index_str, NULL, 10);
            if (file_index < 0 || file_index >= library.num_files) {
                printf("Invalid file index\n");
                continue;
            }

            if (stream_and_get_request(sockfd, file_index, &library) == -1) {
                goto error;
            }

        } else if (strcmp(command, CMD_HELP) == 0) {
            _print_shell_help();

        } else if (strcmp(command, CMD_QUIT) == 0) {
            printf("Quitting shell\n");
            break;

        } else {
            printf("Invalid command\n");
        }
    }

    _free_library(&library);
    return 0;
error:
    _free_library(&library);
    return -1;
}


static void print_usage() {
    printf("Usage: as_client [-h] [-a NETWORK_ADDRESS] [-p PORT] [-l LIBRARY_DIRECTORY]\n");
    printf("  -h: Print this help message\n");
    printf("  -a NETWORK_ADDRESS: Connect to server at NETWORK_ADDRESS (default 'localhost')\n");
    printf("  -p  Port to listen on (default: " XSTR(DEFAULT_PORT) ")\n");
    printf("  -l LIBRARY_DIRECTORY: Use LIBRARY_DIRECTORY as the library directory (default 'as-library')\n");
}


int main(int argc, char * const *argv) {
    int opt;
    int port = DEFAULT_PORT;
    const char *hostname = "localhost";
    const char *library_directory = "saved";

    while ((opt = getopt(argc, argv, "ha:p:l:")) != -1) {
        switch (opt) {
            case 'h':
                print_usage();
                return 0;
            case 'a':
                hostname = optarg;
                break;
            case 'p':
                port = strtol(optarg, NULL, 10);
                if (port < 0 || port > 65535) {
                    ERR_PRINT("Invalid port number %d\n", port);
                    return 1;
                }
                break;
            case 'l':
                library_directory = optarg;
                break;
            default:
                print_usage();
                return 1;
        }
    }

    printf("Connecting to server at %s:%d, using library in %s\n",
           hostname, port, library_directory);

    int sockfd = connect_to_server(port, hostname);
    if (sockfd == -1) {
        return -1;
    }

    int result = client_shell(sockfd, library_directory);
    if (result == -1) {
        close(sockfd);
        return -1;
    }

    close(sockfd);
    return 0;
}

# File System Scanner

## Function Reference

### Queue Management

- `queue_init`: Initializes the work queue with specified capacity and mutex/condition variables
- `queue_destroy`: Frees all resources associated with the work queue
- `queue_resize`: Doubles the queue capacity when it becomes full
- `queue_push`: Adds a new path to the work queue
- `queue_pop`: Removes and returns a path from the work queue

### Thread Management

- `worker_thread`: Main thread function that processes directories and files
- `check_termination_condition`: Checks if scanning should terminate based on queue state
- `handle_signal`: Handles interrupt signals for graceful shutdown

### File Processing

- `process_file`: Processes a single file and writes its information to output
- `matches_extension`: Checks if a file matches the specified extensions
- `matches_size`: Verifies if file size is within specified bounds
- `matches_owner`: Checks if file owner matches the filter
- `matches_group`: Checks if file group matches the filter
- `matches_mtime`: Verifies if file modification time is within specified range
- `matches_permissions`: Checks if file permissions match the filter

## Arguments:

- `directory`: The starting directory to scan
- `output_file`: Path where results will be saved
- `output_file_type`: Format of output (csv, json, or text)

## Optional Filters:

- `extension <num> <ext1> ... <extN>`: Filter by file extensions
- `minSize <size>`: Minimum file size in bytes
- `maxSize <size>`: Maximum file size in bytes
- `owner <owner_name>`: Filter by file owner
- `group <group_name>`: Filter by file group
- `mtime_after <YYYY-MM-DD>`: Files modified after date
- `mtime_before <YYYY-MM-DD>`: Files modified before date
- `permissions <octal_perm>`: Filter by exact permission match

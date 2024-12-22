# File System Scanner

## Features Overview

The Concurrent File System Scanner is a multi-threaded tool designed for efficient file system scanning with advanced filtering and output capabilities. It supports recursive directory traversal, concurrent processing via a thread pool, and detailed file metadata tracking. Graceful termination is implemented with signal handling, and memory-efficient methods ensure scalability.

### Key Features

**Enhanced Filtering**:

- Filter by file extensions (supports multiple extensions).
- Size-based filters with minimum and maximum thresholds.
- Filters based on file owner, group, and permissions (octal format).
- Date range filters for modification times.

**Advanced Output**:

- Supports CSV, JSON, and plain text formats.
- Structured and consistently formatted output.
- Real-time output flushing for large datasets.

The scanner expands upon the original proposal by introducing comprehensive filtering, versatile output options, and robust error handling, making it highly adaptable for production environments while maintaining its core concurrency features.

### How to Run

```bash
./scanner <directory> <output_file> <output_file_type> [options]  
```

Optional filters include file extension, size, owner, group, modification time, and permissions. The scanner is designed for ease of use and high performance, offering structured outputs suitable for analysis or integration.

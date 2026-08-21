CREATE DATABASE IF NOT EXISTS net_disk
    DEFAULT CHARACTER SET utf8
    COLLATE utf8_general_ci;

USE net_disk;

CREATE TABLE IF NOT EXISTS client_info (
    id INT AUTO_INCREMENT,
    username VARCHAR(100),
    salt CHAR(25),
    cryptpasswd VARCHAR(100),
    pwd VARCHAR(255),
    code INT,
    PRIMARY KEY (id),
    UNIQUE KEY (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS local_files (
    id INT AUTO_INCREMENT,
    real_file_name VARCHAR(255),
    link_num INT,
    md5 CHAR(40),
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS virtual_file (
    id INT AUTO_INCREMENT,
    parent_id INT,
    filename VARCHAR(255),
    owner_id INT,
    md5 CHAR(40),
    filesize INT,
    type CHAR(1),
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

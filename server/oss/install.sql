-- ============================================================================
--  VRATA – strežniška baza (MySQL 8.0)
--  Uvoz: phpMyAdmin -> izberite bazo -> Uvozi -> ta datoteka.
--  Vse tabele imajo predpono vr_, zato ne trčijo s starimi tabelami.
--  Časi so shranjeni v UTC (stran jih prikaže po ljubljanskem času).
-- ============================================================================
SET NAMES utf8mb4;
SET time_zone = '+00:00';

CREATE TABLE vr_roles (
  id          INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  name        VARCHAR(64)  NOT NULL UNIQUE,
  description VARCHAR(255) NOT NULL DEFAULT '',
  is_system   TINYINT(1)   NOT NULL DEFAULT 0   -- 1 = Administrator: vse pravice, ni urejljiva
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE vr_role_permissions (
  role_id INT UNSIGNED NOT NULL,
  perm    VARCHAR(40)  NOT NULL,
  PRIMARY KEY (role_id, perm),
  FOREIGN KEY (role_id) REFERENCES vr_roles(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE vr_users (
  id            INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  username      VARCHAR(64)  NOT NULL UNIQUE,
  display_name  VARCHAR(100) NOT NULL DEFAULT '',
  password_hash VARCHAR(255) NOT NULL,
  role_id       INT UNSIGNED NOT NULL,
  all_doors     TINYINT(1)   NOT NULL DEFAULT 1,   -- 0 = samo izbrane skupine/vrata
  active        TINYINT(1)   NOT NULL DEFAULT 1,
  created_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_login_at DATETIME     NULL,
  FOREIGN KEY (role_id) REFERENCES vr_roles(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE vr_groups (
  id         INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  name       VARCHAR(64)   NOT NULL UNIQUE,
  ical_url   VARCHAR(1000) NOT NULL DEFAULT '',
  disabled   TINYINT(1)    NOT NULL DEFAULT 0,
  created_at DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE vr_firmware (
  id          INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  version     VARCHAR(32)  NOT NULL,
  filename    VARCHAR(255) NOT NULL,
  size        INT UNSIGNED NOT NULL,
  md5         CHAR(32)     NOT NULL,
  data        LONGBLOB     NOT NULL,
  notes       VARCHAR(255) NOT NULL DEFAULT '',
  uploaded_by INT UNSIGNED NULL,
  uploaded_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE vr_doors (
  id              INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  name            VARCHAR(32)   NOT NULL UNIQUE,      -- ime naprave (V1 ...)
  description     VARCHAR(255)  NOT NULL DEFAULT '',
  group_id        INT UNSIGNED  NULL,
  mac             CHAR(17)      NULL UNIQUE,
  token_hash      CHAR(64)      NULL,                 -- SHA-256 ključa vrat
  token_plain     CHAR(64)      NULL,                 -- samo do prve predaje po sprejemu
  pair_code       VARCHAR(8)    NULL,                 -- koda, ki jo vrata kažejo na svoji strani
  status          TINYINT       NOT NULL DEFAULT 2,   -- 0 LOCK, 1 OPEN, 2 SCHEDULE REMOTE, 3 SCHEDULE LOCAL
  open_until      DATETIME      NULL,                 -- takojšnji odklep do tega časa
  local_ical_url  VARCHAR(1000) NOT NULL DEFAULT '',
  poll_s          INT           NOT NULL DEFAULT 60,  -- preverjanje strežnika (30–3600 s)
  calendar_s      INT           NOT NULL DEFAULT 300, -- prenos koledarja (60–3600 s)
  setup_ap        ENUM('auto','button') NOT NULL DEFAULT 'auto',
  wifi_rev        INT           NOT NULL DEFAULT 0,   -- 0 = Wi-Fi ureja samo naprava
  wifi_json       TEXT          NULL,
  portal_enabled  TINYINT(1)    NOT NULL DEFAULT 0,
  portal_ssid     VARCHAR(32)   NOT NULL DEFAULT 'Vrata',
  portal_unlock_s INT           NOT NULL DEFAULT 600,
  portal_pause_s  INT           NOT NULL DEFAULT 540, -- omrežje portala izklopljeno po odklepu (0 = ne)
  pin_before_min  INT           NOT NULL DEFAULT 15,
  pin_after_min   INT           NOT NULL DEFAULT 15,
  firmware_id     INT UNSIGNED  NULL,
  config_rev      INT UNSIGNED  NOT NULL DEFAULT 1,
  -- zadnje poročilo vrat
  last_seen_at    DATETIME      NULL,
  last_ip         VARCHAR(45)   NULL,
  fw_version      VARCHAR(32)   NULL,
  boot_id         VARCHAR(16)   NULL,
  report          JSON          NULL,
  created_at      DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (group_id)    REFERENCES vr_groups(id)   ON DELETE SET NULL,
  FOREIGN KEY (firmware_id) REFERENCES vr_firmware(id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Nova vrata, ki so se oglasila, a jih administrator še ni sprejel.
CREATE TABLE vr_pending (
  mac           CHAR(17)    NOT NULL PRIMARY KEY,
  pair_code     VARCHAR(8)  NOT NULL,
  ip            VARCHAR(45) NULL,
  local_ip      VARCHAR(45) NULL,
  fw_version    VARCHAR(32) NULL,
  first_seen_at DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_seen_at  DATETIME    NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE vr_user_groups (
  user_id  INT UNSIGNED NOT NULL,
  group_id INT UNSIGNED NOT NULL,
  PRIMARY KEY (user_id, group_id),
  FOREIGN KEY (user_id)  REFERENCES vr_users(id)  ON DELETE CASCADE,
  FOREIGN KEY (group_id) REFERENCES vr_groups(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE vr_user_doors (
  user_id INT UNSIGNED NOT NULL,
  door_id INT UNSIGNED NOT NULL,
  PRIMARY KEY (user_id, door_id),
  FOREIGN KEY (user_id) REFERENCES vr_users(id) ON DELETE CASCADE,
  FOREIGN KEY (door_id) REFERENCES vr_doors(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Stalni uporabniki portala (telovadnica); rezervacije pridejo iz koledarja.
CREATE TABLE vr_portal_users (
  id           INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  door_id      INT UNSIGNED NOT NULL,
  username     VARCHAR(20)  NOT NULL,
  display_name VARCHAR(100) NOT NULL DEFAULT '',
  pin          VARCHAR(8)   NOT NULL,
  active       TINYINT(1)   NOT NULL DEFAULT 1,
  created_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uq_door_user (door_id, username),
  FOREIGN KEY (door_id) REFERENCES vr_doors(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE vr_events (
  id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  created_at DATETIME     NOT NULL,
  door_id    INT UNSIGNED NULL,
  user_id    INT UNSIGNED NULL,
  type       VARCHAR(24)  NOT NULL,
  message    VARCHAR(500) NOT NULL,
  KEY idx_door_time (door_id, created_at),
  KEY idx_time (created_at),
  FOREIGN KEY (door_id) REFERENCES vr_doors(id) ON DELETE SET NULL,
  FOREIGN KEY (user_id) REFERENCES vr_users(id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE vr_login_attempts (
  id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  ip         VARCHAR(45) NOT NULL,
  username   VARCHAR(64) NOT NULL,
  success    TINYINT(1)  NOT NULL,
  created_at DATETIME    NOT NULL,
  KEY idx_ip (ip, created_at),
  KEY idx_user (username, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Privzete vloge (pravice lahko pozneje spreminjate na strani Vloge).
INSERT INTO vr_roles (id, name, description, is_system) VALUES
  (1, 'Administrator', 'Vse pravice', 1),
  (2, 'Osebje', 'Status vrat, koledarji in uporabniki telovadnice', 0),
  (3, 'Pregled', 'Samo ogled vrat', 0);

INSERT INTO vr_role_permissions (role_id, perm) VALUES
  (2, 'doors.view'), (2, 'status.edit'), (2, 'calendar.edit'), (2, 'portal.users'),
  (3, 'doors.view');

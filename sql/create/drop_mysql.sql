REVOKE ALL PRIVILEGES ON * . * FROM 'tournament'@'localhost';

REVOKE ALL PRIVILEGES ON `world` . * FROM 'tournament'@'localhost';

REVOKE GRANT OPTION ON `world` . * FROM 'tournament'@'localhost';

REVOKE ALL PRIVILEGES ON `characters` . * FROM 'tournament'@'localhost';

REVOKE GRANT OPTION ON `characters` . * FROM 'tournament'@'localhost';

REVOKE ALL PRIVILEGES ON `auth` . * FROM 'tournament'@'localhost';

REVOKE GRANT OPTION ON `auth` . * FROM 'tournament'@'localhost';

DROP USER 'tournament'@'localhost';

DROP DATABASE IF EXISTS `world`;

DROP DATABASE IF EXISTS `characters`;

DROP DATABASE IF EXISTS `auth`;

SHOW ARCHITECTURE;
CREATE TABLE events (id INT, text TEXT);
BEGIN;
INSERT INTO events VALUES (1, 'will be rolled back');
SELECT * FROM events;
ROLLBACK;
SELECT * FROM events;
VACUUM events;
QUIT;

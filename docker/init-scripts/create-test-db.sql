-- Runs once, on first container init (docker-entrypoint-initdb.d convention).
-- Keeps the test suite's database (lexis_test) fully separate from the
-- real application database (lexis, created via POSTGRES_DB) -- tests
-- TRUNCATE their tables between runs, and that must never touch real
-- ingested data.
CREATE DATABASE lexis_test;

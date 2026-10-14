-- The tables tests/integration/test_gen_e2e.cpp creates, in the same order it
-- creates them, so --input-ddl answers for the golden header beside this file.
CREATE TABLE uniorm_gen_user (
  id BIGINT NOT NULL PRIMARY KEY,
  name VARCHAR(64) NOT NULL,
  age INT NULL,
  created TIMESTAMP NULL
);
CREATE TABLE uniorm_gen_order (
  id BIGINT NOT NULL PRIMARY KEY,
  user_id BIGINT NOT NULL,
  amount DECIMAL(10,2) NOT NULL,
  note VARCHAR(128) NULL,
  CONSTRAINT fk_gen_order_user FOREIGN KEY (user_id)
    REFERENCES uniorm_gen_user (id)
);
CREATE INDEX idx_gen_order_user ON uniorm_gen_order (user_id);

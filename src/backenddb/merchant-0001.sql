--
-- This file is part of TALER
-- Copyright (C) 2014--2020 Taler Systems SA
--
-- TALER is free software; you can redistribute it and/or modify it under the
-- terms of the GNU General Public License as published by the Free Software
-- Foundation; either version 3, or (at your option) any later version.
--
-- TALER is distributed in the hope that it will be useful, but WITHOUT ANY
-- WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
-- A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License along with
-- TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
--

-- Everything in one big transaction
BEGIN;

-- TODO: consider adding BIGSERIAL primary keys on many of the tables!

-- Check patch versioning is in place.
SELECT _v.register_patch('merchant-0001', NULL, NULL);

---------------- Exchange information ---------------------------

CREATE TABLE IF NOT EXISTS merchant_exchange_wire_fees
  (master_pub BYTEA NOT NULL CHECK (LENGTH(master_pub)=32)
  ,h_wire_method BYTEA NOT NULL CHECK (LENGTH(h_wire_method)=64)
  ,wire_fee_val INT8 NOT NULL
  ,wire_fee_frac INT4 NOT NULL
  ,closing_fee_val INT8 NOT NULL
  ,closing_fee_frac INT4 NOT NULL
  ,start_date INT8 NOT NULL
  ,end_date INT8 NOT NULL
  ,master_sig BYTEA NOT NULL CHECK (LENGTH(master_sig)=64)
  ,PRIMARY KEY (exchange_pub,h_wire_method,start_date,end_date)
  );
COMMENT ON TABLE merchant_exchange_wire_fees
 IS 'Here we store proofs of the wire fee structure of the various exchanges';
COMMENT ON COLUMN merchant_exchange_wire_fees.master_pub
 IS 'Master public key of the exchange with these wire fees';

CREATE TABLE IF NOT EXISTS merchant_exchange_signing_keys
  (master_pub BYTEA NOT NULL CHECK (LENGTH(master_pub)=32)
  ,exchange_pub BYTEA NOT NULL CHECK (LENGTH(exchange_pub)=32)
  ,start_date INT8 NOT NULL
  ,expire_date INT8 NOT NULL
  ,end_date INT8 NOT NULL
  ,master_sig BYTEA NOT NULL CHECK (LENGTH(master_sig)=64)
  ,PRIMARY KEY (master_pub,exchange_pub)
  );
COMMENT ON TABLE merchant_exchange_signing_keys
 IS 'Here we store proofs of the exchange online signing keys being signed by the exchange master key';
COMMENT ON COLUMN merchant_exchange_signing_keys.master_pub
 IS 'Master public key of the exchange with these online signing keys';


-------------------------- Instances  ---------------------------

CREATE TABLE IF NOT EXISTS merchant_instances
  (merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32),
  ,merchant_name VARCHAR NOT NULL
  ,location BYTEA NOT NULL
  ,jurisdiction BYTEA NOT NULL
  ,PRIMARY KEY (reserve_pub)
  );
COMMENT ON TABLE merchant_instances
  IS 'all the instances supported by this backend';
COMMENT ON COLUMN merchant_instances.merchant_name
  IS 'legal name of the merchant as a simple string (required)';
COMMENT ON COLUMN merchant_instances.location
  IS 'physical location of the merchant as a Location in JSON format (required)';
COMMENT ON COLUMN merchant_instances.jurisdiction
  IS 'jurisdiction of the merchant as a Location in JSON format (required)';

CREATE TABLE IF NOT EXISTS merchant_keys
  (merchant_priv BYTEA NOT NULL CHECK (LENGTH(merchant_priv)=32) UNIQUE,
   merchant_pub BYTEA NOT NULL
     REFERENCES merchant_instances (merchant_pub) ON DELETE CASCADE
  ,PRIMARY KEY (merchant_pub)
  );
COMMENT ON TABLE merchant_keys
  IS 'private keys of instances that have not been deleted';

CREATE TABLE IF NOT EXISTS merchant_instance_accounts
  (account_id BIGSERIAL NOT NULL
   merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32),
  ,h_wire BYTEA NOT NULL CHECK (LENGTH(h_wire)=64), -- or did we use a shorter hash here?
  ,salt BYTEA NOT NULL CHECK (LENGTH(salt)=64), -- or did we use a shorter salt here?
  ,active boolean NOT NULL
  ,payto_uri VARCHAR NOT NULL CHECK,
  ,PRIMARY KEY (merchant_pub,h_wire)
  ,FOREIGN KEY (merchant_pub)
    REFERENCES merchant_instances (merchant_pub) ON DELETE CASCADE
  );
COMMENT ON TABLE merchant_accounts
  IS 'bank accounts of the instances';
COMMENT ON COLUMN merchant_accounts.h_wire
  IS 'salted hash of payto_uri';
COMMENT ON COLUMN merchant_accounts.salt
  IS 'salt used when hashing payto_uri into h_wire';
COMMENT ON COLUMN merchant_accounts.payto_uri
  IS 'payto URI of a merchant bank account';
COMMENT ON COLUMN merchant_instances.active
  IS 'true if we actively use this bank account, false if it is just kept around for older contracts to refer to';


-------------------------- Inventory  ---------------------------

CREATE TABLE IF NOT EXISTS merchant_inventory
  (product_id VARCHAR NOT NULL
  ,merchant_pub BYTEA NOT NULL
    REFERENCES merchant_instances (merchant_pub) ON DELETE CASCADE
  ,description VARCHAR NOT NULL
  ,description_i18n BYTEA NOT NULL
  ,unit VARCHAR NOT NULL
  ,image BYTEA NOT NULL
  ,taxes BYTEA NOT NULL
  ,price_val INT8 NOT NULL
  ,price_frac INT4 NOT NULL
  ,total_stock BIGINT NOT NULL
  ,total_sold BIGINT NOT NULL
  ,total_lost BIGINT NOT NULL
  ,location BYTEA NOT NULL
  ,next_restock INT8 NOT NULL
  ,PRIMARY KEY (product_id, merchant_pub)
  );
COMMENT ON TABLE merchant_inventory
  IS 'products offered by the merchant (may be incomplete, frontend can override)';
COMMENT ON COLUMN merchant_inventory.description
  IS 'Human-readable product description';
COMMENT ON COLUMN merchant_inventory.description_i18n
  IS 'JSON map from IETF BCP 47 language tags to localized descriptions';
COMMENT ON COLUMN merchant_inventory.unit
  IS 'Unit of sale for the product (liters, kilograms, packages)';
COMMENT ON COLUMN merchant_inventory.image
  IS 'NOT NULL, but can be 0 bytes; must contain an ImageDataUrl';
COMMENT ON COLUMN merchant_inventory.taxes
  IS 'JSON array containing taxes the merchant pays, must be JSON, but can be just "[]"';
COMMENT ON COLUMN merchant_inventory.price_val
  IS 'Current price of one unit of the product';
COMMENT ON COLUMN merchant_inventory.total_stock
  IS 'A value of -1 is used for unlimited (eletronic good), may never be lowered';
COMMENT ON COLUMN merchant_inventory.total_sold
  IS 'Number of products sold, must be below total_stock, non-negative, may never be lowered';
COMMENT ON COLUMN merchant_inventory.total_lost
  IS 'Number of products that used to be in stock but were lost (spoiled, damaged), may never be lowered';
COMMENT ON COLUMN merchant_inventory.location
  IS 'JSON formatted Location of where the product is stocked';
COMMENT ON COLUMN merchant_inventory.next_restock
  IS 'GNUnet absolute time indicating when the next restock is expected. 0 for unknown.';

CREATE TABLE IF NOT EXISTS merchant_inventory_locks
  (product_id VARCHAR NOT NULL
  ,merchant_pub BYTEA NOT NULL
     REFERENCES merchant_instances (merchant_pub) ON DELETE CASCADE
  ,lock_uuid BYTEA NOT NULL -- FIXME: length constraint?
  ,total_locked BIGINT NOT NULL
  ,expiration TIMESTAMP NOT NULL,
  ,FOREIGN KEY (product_id, merchant_pub)
     REFERENCES merchant_inventory (product_id, merchant_pub) ON DELETE CASCADE
  );
COMMENT ON TABLE merchant_inventory_locks
  IS 'locks on inventory helt by shopping carts';
COMMENT ON TABLE merchant_inventory_locks.total_locked
  IS 'how many units of the product does this lock reserve';
COMMENT ON TABLE merchant_inventory_locks.expiration
  IS 'when does this lock automatically expire (if no order is created)';


---------------- Orders and contracts ---------------------------

CREATE TABLE IF NOT EXISTS merchant_orders
  (order_id VARCHAR NOT NULL
  ,merchant_pub BYTEA NOT NULL
    REFERENCES merchant_instances (merchant_pub) ON DELETE CASCADE
  ,contract_terms BYTEA NOT NULL
  ,pay_deadline INT8 NOT NULL
  ,PRIMARY KEY (order_id, merchant_pub)
  );
COMMENT ON TABLE merchant_orders
  IS 'Orders we offered to a customer, but that have not yet been claimed';
COMMENT ON COLUMN merchnat_orders.contract_terms
  IS 'Claiming changes the contract_terms, hence we have no hash of the terms in this table';
COMMENT ON COLUMN merchant_orders.merchant_pub
  IS 'Identifies the instance offering the contract';
COMMENT ON COLUMN merchant_orders.pay_deadline
  IS 'How long is the offer valid. After this time, the order can be garbage collected';
CREATE INDEX IF NOT EXISTS merchant_orders_by_expiration
  ON merchant_orders
    (pay_deadline);

CREATE TABLE IF NOT EXISTS merchant_order_locks
  (product_id VARCHAR NOT NULL
  ,merchant_pub BYTEA NOT NULL
     REFERENCES merchant_instances (merchant_pub) ON DELETE CASCADE
  ,total_locked BIGINT NOT NULL
  ,order_id VARCHAR NOT NULL,
  ,FOREIGN KEY (order_id, merchant_pub)
     REFERENCES merchant_orders (order_id, merchant_pub) ON DELETE CASCADE
  ,FOREIGN KEY (product_id, merchant_pub)
     REFERENCES merchant_inventory (product_id, merchant_pub) ON DELETE CASCADE
  ,PRIMARY KEY (product_id, merchant_pub, order_id)
  );
COMMENT ON TABLE merchant_inventory_locks
  IS 'locks on orders awaiting claim and payment';
COMMENT ON TABLE merchant_inventory_locks.total_locked
  IS 'how many units of the product does this lock reserve';

CREATE TABLE IF NOT EXISTS merchant_contract_terms
  (order_id VARCHAR NOT NULL
  ,merchant_pub BYTEA NOT NULL
    REFERENCES merchant_instances (merchant_pub) ON DELETE CASCADE
  ,contract_terms BYTEA NOT NULL
  ,h_contract_terms BYTEA NOT NULL CHECK (LENGTH(h_contract_terms)=64)
  ,pay_deadline INT8 NOT NULL
  ,row_id BIGSERIAL UNIQUE
  ,paid boolean DEFAULT FALSE NOT NULL
  ,refund_deadline INT8 NOT NULL
  ,PRIMARY KEY (order_id, merchant_pub)
  ,UNIQUE (h_contract_terms, merchant_pub)
  );
COMMENT ON TABLE merchant_contract_terms
  IS 'Contracts are orders that have been claimed by a wallet';
COMMENT ON COLUMN merchant_contract_terms.order_id
  IS 'Not a foreign key into merchant_orders because paid contracts persist after expiration';
COMMENT ON COLUMN merchant_contract_terms.merchant_pub
  IS 'Identifies the instance offering the contract';
COMMENT ON COLUMN merchant_contract_terms.contract_terms
  IS 'These contract terms include the wallet nonce';
COMMENT ON COLUMN merchant_contract_terms.h_contract_terms
  IS 'Hash over contract_terms';
COMMENT ON COLUMN merchant_contract_terms.refund_deadline
  IS 'By what times do refunds have to be approved (useful to reject refund requests)';
COMMENT ON COLUMN merchant_contract_terms.paid
  IS 'true implies the customer paid for this contract; order should be DELETEd from merchant_orders once paid is set to release merchant_order_locks; paid remains true even if the payment was later refunded';
COMMENT ON COLUMN merchant_contract_terms.pay_deadline
  IS 'How long is the offer valid. After this time, the order can be garbage collected';
CREATE INDEX IF NOT EXISTS merchant_contract_terms_by_expiration
  ON merchant_contract_terms
  (pay_deadline);


---------------- Payment and refunds ---------------------------

CREATE TABLE IF NOT EXISTS merchant_deposits
  (h_contract_terms BYTEA NOT NULL
  ,merchant_pub BYTEA NOT NULL
     REFERENCES merchant_instances (merchant_pub) ON DELETE CASCADE
  ,coin_pub BYTEA NOT NULL CHECK (LENGTH(coin_pub)=32)
  ,exchange_url VARCHAR NOT NULL
  ,amount_with_fee_val INT8 NOT NULL
  ,amount_with_fee_frac INT4 NOT NULL
  ,deposit_fee_val INT8 NOT NULL
  ,deposit_fee_frac INT4 NOT NULL
  ,refund_fee_val INT8 NOT NULL
  ,refund_fee_frac INT4 NOT NULL
  ,wire_fee_val INT8 NOT NULL
  ,wire_fee_frac INT4 NOT NULL
  ,exchange_pub BYTEA NOT NULL
     REFERENCES merchant_exchange_signing_keys (exchange_pub) ON DELETE CASCADE
  ,exchange_sig BYTEA NOT NULL CHECK (LENGTH(exchange_sig)=64)
  ,exchange_timestamp INT 8 NOT NULL
  ,h_wire BYTEA NOT NULL
  ,exchange_proof BYTEA NOT NULL
  ,PRIMARY KEY (h_contract_terms, coin_pub)
  ,FOREIGN KEY (h_contract_terms, merchant_pub)
     REFERENCES merchant_contract_terms (h_contract_terms, merchant_pub) ON DELETE CASCADE
  ,FOREIGN KEY (h_wire, merchant_pub)
     REFERENCES merchant_instance_accounts (h_wire, merchant_pub) ON DELETE CASCADE
  );
COMMENT ON TABLE merchant_deposits
  IS 'Table with the deposit confirmations for each coin we deposited at the exchange';
COMMENT ON COLUMN merchant_deposits.exchange_pub
  IS 'Online signing key of the exchange on the deposit confirmation';
COMMENT ON COLUMN merchant_deposits.exchange_sig
  IS 'Signature of the exchange over the deposit confirmation';
COMMENT ON COLUMN merchant_deposits.wire_fee_val
  IS 'We MAY want to see if we should try to get this via merchant_exchange_wire_fees (not sure, may be too complicated with the date range, etc.)';
COMMENT ON COLUMN merchant_deposits.transferred

CREATE TABLE IF NOT EXISTS merchant_session_info
  (session_id VARCHAR NOT NULL
  ,fulfillment_url VARCHAR NOT NULL
-- FIXME: why do we store this here?
  ,order_id VARCHAR NOT NULL
  ,merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)
  ,timestamp INT8 NOT NULL
  ,PRIMARY KEY (session_id, fulfillment_url, merchant_pub)
-- FIXME: I am confused why this even *IS* a primary key.
  ,FOREIGN KEY (order_id, merchant_pub)
     REFERENCES merchant_orders (order_id, merchant_pub)
-- FIXME: if this is for session-bound payments,
--        we need to reference merchant_contracts as
--        the *order* may be GCed after payment but the
--        session-bound payment mechanism should continue to work!
  ,UNIQUE (session_id, fulfillment_url, order_id, merchant_pub)
-- FIXME: isn't this redundant with the (confusing) PRIMARY KEY?
  );
-- FIXME: Support for #5853 (limit session in number and duration)
--        should be supported 'somewhere' here.
-- => #5853 suggests a UNIQUE constraint on order_id+merchant_pub on this table!
COMMENT ON TABLE merchant_session_info
  IS 'sessions and their order_id/fulfillment_url mapping';
COMMENT ON COLUMN merchant_session_info.fulfillment_url
  IS 'FIXME: Unclear why the fulfillment URL is in this table';
COMMENT ON COLUMN merchant_session_info.order_id
  IS 'FIXME: Why use merchant_pub+order_id here, instead of a say a contract_id?';

CREATE TABLE IF NOT EXISTS merchant_refunds
  (rtransaction_id BIGSERIAL UNIQUE
  ,merchant_pub BYTEA NOT NULL
  ,h_contract_terms BYTEA NOT NULL
  ,coin_pub BYTEA NOT NULL
  ,reason VARCHAR NOT NULL
  ,refund_amount_val INT8 NOT NULL
  ,refund_amount_frac INT4 NOT NULL
  ,FOREIGN KEY (h_contract_terms, coin_pub)
     REFERENCES merchant_deposits (h_contract_terms, coin_pub) ON DELETE CASCADE
  ,FOREIGN KEY (h_contract_terms, merchant_pub)
     REFERENCES merchant_contract_terms (h_contract_terms, merchant_pub) ON DELETE CASCADE
  ,PRIMARY KEY (h_contract_terms, merchant_pub, coin_pub, rtransaction_id)
  );
COMMENT ON TABLE merchant_deposits
  IS 'Refunds approved by the merchant (backoffice) logic, excludes abort refunds';
COMMENT ON COLUMN merchant_refunds.rtransaction_id
  IS 'Needed for uniqueness in case a refund is increased for the same order';
-- FIXME: do we really want rtransaction_id as BIGSERIAL UNIQUE?
--        this exposes # of refunds granted to clients!!!

CREATE TABLE IF NOT EXISTS merchant_refund_proofs
  (rtransaction_id BIGSERIAL UNIQUE
  ,merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)
  ,h_contract_terms BYTEA NOT NULL CHECK (LENGTH(h_contract_terms)=64)
  ,coin_pub BYTEA NOT NULL CHECK (LENGTH(coin_pub)=32)
  ,exchange_sig BYTEA NOT NULL CHECK (LENGTH(exchange_sig)=64)
  ,exchange_pub BYTEA NOT NULL
     REFERENCES merchant_exchange_signing_keys (exchange_pub) ON DELETE CASCADE
  ,FOREIGN KEY (h_contract_terms, merchant_pub, coin_pub, rtransaction_id)
     REFERENCES merchant_refunds (h_contract_terms, merchant_pub, coin_pub, rtransaction_id) ON DELETE CASCADE
  ,PRIMARY KEY (h_contract_terms, merchant_pub, coin_pub, rtransaction_id)
);
COMMENT ON TABLE merchant_refund_proofs
  IS 'Refunds confirmed by the exchange (not all approved refunds are grabbed by the wallet)';
-- FIXME: rtransaction_id as BIGSERIAL UNIQUE should suffice, rest of information
--        in the FOREIGN KEY is duplicated for no good reason.


-------------------- Wire transfers ---------------------------

CREATE TABLE IF NOT EXISTS merchant_credits
  (credit_serial BIGSERIAL NOT NULL
  ,exchange_url VARCHAR NOT NULL
  ,wtid BYTEA CHECK (LENGTH(wtid)=32)
  ,credit_amount_val INT8 NOT NULL
  ,credit_amount_frac INT4 NOT NULL
  ,account_id BIGSERIAL NOT NULL
     REFERENCES merchant_instance_accounts (account_id) ON DELETE CASCADE
  ,verified BOOLEAN NOT NULL DEFAULT FALSE
  ,PRIMARY KEY (wtid, exchange_url)
  );
COMMENT ON TABLE merchant_credits
  IS 'table represents the information provided by the (trusted) merchant about incoming wire transfers';
COMMENT ON COLUMN merchant_credits.verified
  IS 'true once we got an acceptable response from the exchange for this transfer';

CREATE TABLE IF NOT EXISTS merchant_transfer_signatures
  (credit_serial BIGSERIAL NOT NULL
  ,execution_time INT8 NOT NULL
  ,exchange_pub BYTEA NOT NULL
     REFERENCES merchant_exchange_signing_keys (exchange_pub) ON DELETE CASCADE
  ,exchange_sig BYTEA NOT NULL CHECK (LENGTH(exchange_sig)=64)
  ,h_wire BYTEA NOT NULL CHECK (LENGTH(h_wire)=64)
  ,PRIMARY KEY (credit_serial)
  ,FOREIGN KEY (credit_serial)
     REFERENCES merchant_credits (credit_serial) ON DELETE CASCADE
  );
COMMENT ON TABLE merchant_transfer_signatures
  IS 'table represents the main information returned from the /transfer request to the exchange.';
COMMENT ON COLUMN merchant_transfer_signatures.h_wire
  IS 'salted hash of the merchant bank account';

CREATE TABLE IF NOT EXISTS merchant_transfer_by_coin
  (h_contract_terms BYTEA NOT NULL
  ,coin_pub BYTEA NOT NULL CHECK (LENGTH(coin_pub)=32)
  ,offset_in_exchange_list INT8 NOT NULL
  ,credit_serial BIGSERIAL NOT NULL
      REFERENCES merchant_credits (credit_serial) ON DELETE CASCADE
  ,exchange_deposit_value_val INT8 NOT NULL
  ,exchange_deposit_value_frac INT4 NOT NULL
  ,exchange_deposit_fee_val INT8 NOT NULL
  ,exchange_deposit_fee_frac INT4 NOT NULL
  ,PRIMARY KEY (h_contract_terms, coin_pub) ON DELETE CASCADE
  );
CREATE INDEX IF NOT EXISTS merchant_transfers_by_coin
  ON merchant_transfers
  (h_contract_terms
  ,coin_pub);
CREATE INDEX IF NOT EXISTS merchant_transfers_by_wtid
  ON merchant_transfers
  (wtid);
COMMENT ON TABLE merchant_transfers
  IS 'Mapping of deposits to wire transfers and vice versa';
COMMENT ON COLUMN merchant_transfers.coin_pub
  IS 'h_contract_terms and coin_pub are not a FOREIGN KEY into merchant_deposits because theoretically the exchange could pay us for things we are not aware of having sold. We still need to store this to check the signatures. This is also the reason why the deposit value and fees are duplicated here: this is about checkability of signatures. We may disagree with the claims of the exchange, but we still need the proof of what the exchange said.';
COMMENT ON COLUMN merchant_transfers.exchange_deposit_value_val
  IS 'Deposit value as claimed by the exchange, should match our values in merchant_deposits minus refunds';
COMMENT ON COLUMN merchant_transfers.exchange_deposit_fee_val
  IS 'Deposit value as claimed by the exchange, should match our values in merchant_deposits';
COMMENT ON COLUMN merchant_transfers.offset_in_exchange_list
  IS 'At which offset in the exchange list for the wire transfer (under "deposits") does this coin appear';
COMMENT ON COLUMN merchant_transfers.coin_pub
  IS 'Note that the coin_pub/h_contract_terms can theoretically be unknown to us if someone else deposited for us at the exchange. Hence those cannot be foreign keys into the merchant_deposits table.';


-------------------------- Tipping ---------------------------

CREATE TABLE IF NOT EXISTS merchant_tip_reserves
  (reserve_pub BYTEA NOT NULL CHECK (LENGTH(reserve_pub)=32)
  ,merchant_pub BYTEA NOT NULL
     REFERENCES merchant_instances (merchant_pub) ON DELETE CASCADE
  ,expiration INT8 NOT NULL
  ,merchant_initial_balance_val INT8 NOT NULL
  ,merchant_initial_balance_frac INT4 NOT NULL
  ,exchange_initial_balance_val INT8 NOT NULL
  ,exchange_initial_balance_frac INT4 NOT NULL
  ,tips_committed_val INT8 NOT NULL
  ,tips_committed_frac INT4 NOT NULL
  ,tips_picked_up_val INT8 NOT NULL
  ,tips_picked_up_frac INT4 NOT NULL
  ,PRIMARY KEY (reserve_pub)
  );
COMMENT ON TABLE merchant_tip_reserves
  IS 'balances of the reserves available for tips';
COMMENT ON COLUMN merchant_tip_reserves.expiration
  IS 'FIXME: EXCHANGE API needs to tell us when reserves close if we are to compute this';
COMMENT ON COLUMN merchant_tip_reserves.merchant_initial_balance_val
  IS 'Set to the initial balance the merchant told us when creating the reserve';
COMMENT ON COLUMN merchant_tip_reserves.exchange_initial_balance_val
  IS 'Set to the initial balance the exchange told us when we queried the reserve status';
COMMENT ON COLUMN merchant_tip_reserves.tips_committed_val
  IS 'Amount of outstanding approved tips that have not been picked up';
COMMENT ON COLUMN merchant_tip_reserves.tips_picked_up_val
  IS 'Total amount tips that have been picked up from this reserve';

CREATE TABLE IF NOT EXISTS merchant_tip_reserve_kreys
  (reserve_priv BYTEA NOT NULL CHECK (LENGTH(reserve_priv)=32) UNIQUE
  ,reserve_pub BYTEA NOT NULL
     REFERENCES merchant_tip_reserves (reserve_pub) ON DELETE CASCADE
  ,exchange_url VARCHAR NOT NULL
  ,PRIMARY KEY (reserve_pub)
  );
COMMENT ON TABLE merchant_tip_reserve_keys
  IS 'private keys of reserves that have not been deleted';

CREATE TABLE IF NOT EXISTS merchant_tips
  (reserve_pub BYTEA NOT NULL
     REFERENCES merchant_tip_reserves (reserve_pub) ON DELETE CASCADE
  ,tip_id BYTEA NOT NULL CHECK (LENGTH(tip_id)=64)
  ,justification VARCHAR NOT NULL
  ,expiration INT8 NOT NULL
  ,amount_val INT8 NOT NULL
  ,amount_frac INT4 NOT NULL
  ,left_val INT8 NOT NULL
  ,left_frac INT4 NOT NULL
  ,was_picked_up BOOLEAN NOT NULL DEFAULT FALSE
  ,PRIMARY KEY (tip_id)
  );
COMMENT ON TABLE merchant_tips
  IS 'tips that have been authorized';
COMMENT ON COLUMN merchant_tips.amount_val
  IS 'Overall tip amount';
COMMENT ON COLUMN merchant_tips.left_val
  IS 'Tip amount not yet picked up';
COMMENT ON COLUMN merchant_tips.reserve_pub
  IS 'Reserve from which this tip is funded';
COMMENT ON COLUMN merchant_tips.expiration
  IS 'time by which the wallet has to pick up the tip before it expires';
CREATE INDEX IF NOT EXISTS merchant_tips_by_expiration
  ON merchant_tips
    (expiration);

CREATE TABLE IF NOT EXISTS merchant_tip_pickups
  (tip_id BYTEA NOT NULL REFERENCES merchant_tips (tip_id) ON DELETE CASCADE
  ,pickup_id BYTEA NOT NULL CHECK (LENGTH(pickup_id)=64)
  ,amount_val INT8 NOT NULL
  ,amount_frac INT4 NOT NULL
  ,PRIMARY KEY (pickup_id)
  );
COMMENT ON TABLE merchant_tip_pickups
  IS 'tips that have been picked up';
COMMENT ON COLUMN merchant_tips.amount_val
  IS 'total transaction cost for all coins including withdraw fees';

CREATE TABLE IF NOT EXISTS merchant_tip_pickup_signatures
  (pickup_id BYTEA NOT NULL REFERENCES merchant_tip_pickups (pickup_id) ON DELETE CASCADE
  ,coin_offset INT4 NOT NULL
  ,blind_sig BYTEA NOT NULL
  ,PRIMARY KEY (pickup_id, coin_offset)
  );
COMMENT ON TABLE merchant_tip_pickup_signatures
  IS 'blind signatures we got from the exchange during the tip pickup';


-- Complete transaction
COMMIT;

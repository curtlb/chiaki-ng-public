-- Example: add a PS account to the rental pool (replace NPSSO locally, never commit real tokens).
-- INSERT INTO CloudStreaming_Accounts (Label, NPSSO, HasPsPlus, Region, Status)
-- VALUES ('ps-rent-01', 'PASTE_NPSSO_HERE', 1, 'PL', 'available');

-- Optional: mark owned game on account (after game exists in CloudStreaming_Games)
-- INSERT INTO CloudStreaming_AccountOwnedGames (AccountID, GameID, EntitlementID)
-- VALUES (1, 1, 'ENTITLEMENT_ID');

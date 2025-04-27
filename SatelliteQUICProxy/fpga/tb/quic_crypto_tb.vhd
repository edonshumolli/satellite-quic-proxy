----------------------------------------------------------------------------------
-- Testbench for QUIC Crypto Module
-- 
-- This testbench validates the functionality of the QUIC cryptographic module
-- by simulating encryption and decryption operations with test vectors.
----------------------------------------------------------------------------------
library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;
use STD.TEXTIO.ALL;
use IEEE.STD_LOGIC_TEXTIO.ALL;

entity quic_crypto_tb is
-- Testbench has no ports
end quic_crypto_tb;

architecture Behavioral of quic_crypto_tb is
    -- Component Declaration for the Unit Under Test (UUT)
    component quic_crypto
        Port (
            clk           : in  STD_LOGIC;
            rst           : in  STD_LOGIC;
            start         : in  STD_LOGIC;
            encrypt       : in  STD_LOGIC;
            done          : out STD_LOGIC;
            data_in       : in  STD_LOGIC_VECTOR(127 downto 0);
            data_in_valid : in  STD_LOGIC;
            data_in_ready : out STD_LOGIC;
            data_out      : out STD_LOGIC_VECTOR(127 downto 0);
            data_out_valid: out STD_LOGIC;
            data_out_ready: in  STD_LOGIC;
            key           : in  STD_LOGIC_VECTOR(127 downto 0);
            nonce         : in  STD_LOGIC_VECTOR(95 downto 0);
            aad           : in  STD_LOGIC_VECTOR(127 downto 0);
            aad_valid     : in  STD_LOGIC;
            aad_ready     : out STD_LOGIC;
            auth_tag_in   : in  STD_LOGIC_VECTOR(127 downto 0);
            auth_tag_out  : out STD_LOGIC_VECTOR(127 downto 0);
            auth_result   : out STD_LOGIC
        );
    end component;
    
    -- Clock and reset signals
    signal clk           : STD_LOGIC := '0';
    signal rst           : STD_LOGIC := '1';
    
    -- Control signals
    signal start         : STD_LOGIC := '0';
    signal encrypt       : STD_LOGIC := '0';
    signal done          : STD_LOGIC;
    
    -- Data interface signals
    signal data_in       : STD_LOGIC_VECTOR(127 downto 0) := (others => '0');
    signal data_in_valid : STD_LOGIC := '0';
    signal data_in_ready : STD_LOGIC;
    signal data_out      : STD_LOGIC_VECTOR(127 downto 0);
    signal data_out_valid: STD_LOGIC;
    signal data_out_ready: STD_LOGIC := '0';
    
    -- Crypto parameter signals
    signal key           : STD_LOGIC_VECTOR(127 downto 0) := (others => '0');
    signal nonce         : STD_LOGIC_VECTOR(95 downto 0) := (others => '0');
    signal aad           : STD_LOGIC_VECTOR(127 downto 0) := (others => '0');
    signal aad_valid     : STD_LOGIC := '0';
    signal aad_ready     : STD_LOGIC;
    signal auth_tag_in   : STD_LOGIC_VECTOR(127 downto 0) := (others => '0');
    signal auth_tag_out  : STD_LOGIC_VECTOR(127 downto 0);
    signal auth_result   : STD_LOGIC;
    
    -- Clock period definition
    constant clk_period  : time := 10 ns;
    
    -- Test data constants
    constant TEST_KEY    : STD_LOGIC_VECTOR(127 downto 0) := x"000102030405060708090A0B0C0D0E0F";
    constant TEST_NONCE  : STD_LOGIC_VECTOR(95 downto 0)  := x"000000000000000000000000";
    constant TEST_AAD    : STD_LOGIC_VECTOR(127 downto 0) := x"00000000000000000000000000000000";
    constant TEST_DATA   : STD_LOGIC_VECTOR(127 downto 0) := x"000102030405060708090A0B0C0D0E0F";
    constant EXPECTED_CIPHER : STD_LOGIC_VECTOR(127 downto 0) := x"58E2FCCEFA7E3061367F1D57A4E7455A";
    constant EXPECTED_TAG   : STD_LOGIC_VECTOR(127 downto 0) := x"A794F1648E5E4B1BC20A6F01AED3AC5D";
    
    -- Test procedure for sending data
    procedure send_data (
        signal data     : in  STD_LOGIC_VECTOR(127 downto 0);
        signal valid    : out STD_LOGIC;
        signal ready    : in  STD_LOGIC;
        signal done_sig : in  STD_LOGIC
    ) is
    begin
        wait until rising_edge(clk);
        valid <= '1';
        wait until ready = '1';
        wait until rising_edge(clk);
        valid <= '0';
        wait until done_sig = '1';
    end procedure;
    
    -- Test procedure for receiving data
    procedure receive_data (
        signal ready    : out STD_LOGIC;
        signal valid    : in  STD_LOGIC;
        signal data     : in  STD_LOGIC_VECTOR(127 downto 0);
        signal expected : in  STD_LOGIC_VECTOR(127 downto 0);
        signal result   : out boolean
    ) is
    begin
        wait until rising_edge(clk);
        ready <= '1';
        wait until valid = '1';
        wait until rising_edge(clk);
        if data = expected then
            result := true;
        else
            result := false;
        end if;
        ready <= '0';
    end procedure;
    
begin
    -- Instantiate the Unit Under Test (UUT)
    uut: quic_crypto
    port map (
        clk            => clk,
        rst            => rst,
        start          => start,
        encrypt        => encrypt,
        done           => done,
        data_in        => data_in,
        data_in_valid  => data_in_valid,
        data_in_ready  => data_in_ready,
        data_out       => data_out,
        data_out_valid => data_out_valid,
        data_out_ready => data_out_ready,
        key            => key,
        nonce          => nonce,
        aad            => aad,
        aad_valid      => aad_valid,
        aad_ready      => aad_ready,
        auth_tag_in    => auth_tag_in,
        auth_tag_out   => auth_tag_out,
        auth_result    => auth_result
    );
    
    -- Clock process definition
    clk_process: process
    begin
        clk <= '0';
        wait for clk_period/2;
        clk <= '1';
        wait for clk_period/2;
    end process;
    
    -- Stimulus process
    stim_proc: process
        variable encryption_result : boolean := false;
        variable decryption_result : boolean := false;
        variable tag_result : boolean := false;
        variable auth_success : boolean := false;
    begin
        -- Initialize inputs
        rst <= '1';
        start <= '0';
        encrypt <= '0';
        data_in <= (others => '0');
        data_in_valid <= '0';
        data_out_ready <= '0';
        key <= (others => '0');
        nonce <= (others => '0');
        aad <= (others => '0');
        aad_valid <= '0';
        auth_tag_in <= (others => '0');
        
        -- Hold reset for 100 ns
        wait for 100 ns;
        rst <= '0';
        wait for clk_period*10;
        
        -- Encryption test
        report "Starting encryption test...";
        key <= TEST_KEY;
        nonce <= TEST_NONCE;
        aad <= TEST_AAD;
        encrypt <= '1';  -- Set to encryption mode
        
        -- Send AAD data
        aad_valid <= '1';
        wait until aad_ready = '1';
        wait until rising_edge(clk);
        aad_valid <= '0';
        
        -- Start encryption
        start <= '1';
        wait until rising_edge(clk);
        start <= '0';
        
        -- Send data to encrypt
        data_in <= TEST_DATA;
        send_data(TEST_DATA, data_in_valid, data_in_ready, done);
        
        -- Receive encrypted data
        data_out_ready <= '1';
        wait until data_out_valid = '1';
        wait until rising_edge(clk);
        
        -- Check if encryption result matches expected
        if data_out = EXPECTED_CIPHER then
            report "Encryption test passed - Encrypted data matches expected value";
            encryption_result := true;
        else
            report "Encryption test failed - Expected: " & to_hstring(EXPECTED_CIPHER) & 
                   " Got: " & to_hstring(data_out) severity error;
        end if;
        
        -- Check authentication tag
        if auth_tag_out = EXPECTED_TAG then
            report "Authentication tag test passed";
            tag_result := true;
        else
            report "Authentication tag test failed - Expected: " & to_hstring(EXPECTED_TAG) & 
                   " Got: " & to_hstring(auth_tag_out) severity error;
        end if;
        
        data_out_ready <= '0';
        wait for clk_period*10;
        
        -- Decryption test
        report "Starting decryption test...";
        encrypt <= '0';  -- Set to decryption mode
        auth_tag_in <= EXPECTED_TAG;  -- Use the expected tag for verification
        
        -- Start decryption
        start <= '1';
        wait until rising_edge(clk);
        start <= '0';
        
        -- Send encrypted data to decrypt
        data_in <= EXPECTED_CIPHER;
        send_data(EXPECTED_CIPHER, data_in_valid, data_in_ready, done);
        
        -- Receive decrypted data
        data_out_ready <= '1';
        wait until data_out_valid = '1';
        wait until rising_edge(clk);
        
        -- Check if decryption result matches original plaintext
        if data_out = TEST_DATA then
            report "Decryption test passed - Decrypted data matches original";
            decryption_result := true;
        else
            report "Decryption test failed - Expected: " & to_hstring(TEST_DATA) & 
                   " Got: " & to_hstring(data_out) severity error;
        end if;
        
        -- Check authentication result
        if auth_result = '1' then
            report "Authentication check passed";
            auth_success := true;
        else
            report "Authentication check failed" severity error;
        end if;
        
        -- Summary
        if encryption_result and decryption_result and tag_result and auth_success then
            report "All tests passed successfully!";
        else
            report "Some tests failed!" severity error;
        end if;
        
        wait;
    end process;

end Behavioral;

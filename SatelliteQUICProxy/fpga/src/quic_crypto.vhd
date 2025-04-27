----------------------------------------------------------------------------------
-- QUIC Encryption/Decryption Module
-- 
-- This module implements the cryptographic functions required for QUIC protocol
-- based on TLS 1.3 cryptographic routines.
--
-- Features:
-- - AES-GCM-128 cipher implementation
-- - AEAD (Authenticated Encryption with Associated Data)
-- - Supporting TLS 1.3 key derivation
----------------------------------------------------------------------------------
library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity quic_crypto is
    Port (
        -- Clock and reset
        clk           : in  STD_LOGIC;
        rst           : in  STD_LOGIC;
        
        -- Control signals
        start         : in  STD_LOGIC;
        encrypt       : in  STD_LOGIC;  -- '1' for encrypt, '0' for decrypt
        done          : out STD_LOGIC;
        
        -- Data interface
        data_in       : in  STD_LOGIC_VECTOR(127 downto 0);
        data_in_valid : in  STD_LOGIC;
        data_in_ready : out STD_LOGIC;
        data_out      : out STD_LOGIC_VECTOR(127 downto 0);
        data_out_valid: out STD_LOGIC;
        data_out_ready: in  STD_LOGIC;
        
        -- Key and nonce
        key           : in  STD_LOGIC_VECTOR(127 downto 0);
        nonce         : in  STD_LOGIC_VECTOR(95 downto 0);
        
        -- Associated data for AEAD
        aad           : in  STD_LOGIC_VECTOR(127 downto 0);
        aad_valid     : in  STD_LOGIC;
        aad_ready     : out STD_LOGIC;
        
        -- Authentication tag
        auth_tag_in   : in  STD_LOGIC_VECTOR(127 downto 0);  -- For verification during decryption
        auth_tag_out  : out STD_LOGIC_VECTOR(127 downto 0);  -- Generated during encryption
        auth_result   : out STD_LOGIC                        -- '1' if authentication passed, '0' otherwise
    );
end quic_crypto;

architecture Behavioral of quic_crypto is
    -- AES-GCM internal components
    component aes_core is
        Port (
            clk        : in  STD_LOGIC;
            rst        : in  STD_LOGIC;
            key        : in  STD_LOGIC_VECTOR(127 downto 0);
            data_in    : in  STD_LOGIC_VECTOR(127 downto 0);
            data_out   : out STD_LOGIC_VECTOR(127 downto 0);
            start      : in  STD_LOGIC;
            done       : out STD_LOGIC
        );
    end component;
    
    component gcm_module is
        Port (
            clk        : in  STD_LOGIC;
            rst        : in  STD_LOGIC;
            h          : in  STD_LOGIC_VECTOR(127 downto 0);  -- Hash subkey
            aad        : in  STD_LOGIC_VECTOR(127 downto 0);  -- Additional authenticated data
            data_in    : in  STD_LOGIC_VECTOR(127 downto 0);
            data_out   : out STD_LOGIC_VECTOR(127 downto 0);
            auth_tag   : out STD_LOGIC_VECTOR(127 downto 0);
            start      : in  STD_LOGIC;
            done       : out STD_LOGIC
        );
    end component;
    
    -- State machine definition
    type state_type is (IDLE, INIT_AES, GEN_HASHKEY, PROCESS_AAD, PROCESS_DATA, FINALIZE, COMPLETE);
    signal state, next_state : state_type;
    
    -- Internal signals
    signal aes_start, aes_done : STD_LOGIC;
    signal aes_data_in, aes_data_out : STD_LOGIC_VECTOR(127 downto 0);
    signal gcm_start, gcm_done : STD_LOGIC;
    signal h_value : STD_LOGIC_VECTOR(127 downto 0);  -- Hash subkey
    signal counter : STD_LOGIC_VECTOR(31 downto 0);
    signal j0 : STD_LOGIC_VECTOR(127 downto 0);       -- Initial counter block
    signal computed_tag : STD_LOGIC_VECTOR(127 downto 0);
    
begin
    -- AES core instantiation
    aes_inst: aes_core
    port map (
        clk       => clk,
        rst       => rst,
        key       => key,
        data_in   => aes_data_in,
        data_out  => aes_data_out,
        start     => aes_start,
        done      => aes_done
    );
    
    -- GCM module instantiation
    gcm_inst: gcm_module
    port map (
        clk       => clk,
        rst       => rst,
        h         => h_value,
        aad       => aad,
        data_in   => data_in,
        data_out  => data_out,
        auth_tag  => computed_tag,
        start     => gcm_start,
        done      => gcm_done
    );
    
    -- State machine process
    process(clk, rst)
    begin
        if rst = '1' then
            state <= IDLE;
            done <= '0';
            data_in_ready <= '0';
            data_out_valid <= '0';
            aad_ready <= '0';
            auth_result <= '0';
        elsif rising_edge(clk) then
            state <= next_state;
            
            case state is
                when IDLE =>
                    done <= '0';
                    if start = '1' then
                        data_in_ready <= '0';
                        aad_ready <= '0';
                    else
                        data_in_ready <= '1';
                        aad_ready <= '1';
                    end if;
                
                when INIT_AES =>
                    -- Prepare to generate hash subkey H
                    aes_data_in <= (others => '0');  -- Encrypt all zeros to get H
                    aes_start <= '1';
                
                when GEN_HASHKEY =>
                    aes_start <= '0';
                    if aes_done = '1' then
                        h_value <= aes_data_out;
                    end if;
                
                when PROCESS_AAD =>
                    aad_ready <= '1';
                    if aad_valid = '1' then
                        -- Process additional authenticated data
                        gcm_start <= '1';
                    end if;
                
                when PROCESS_DATA =>
                    data_in_ready <= '1';
                    gcm_start <= '0';
                    if data_in_valid = '1' then
                        -- Process data blocks
                    end if;
                
                when FINALIZE =>
                    if encrypt = '1' then
                        auth_tag_out <= computed_tag;
                    else
                        -- Compare computed tag with received tag
                        if computed_tag = auth_tag_in then
                            auth_result <= '1';
                        else
                            auth_result <= '0';
                        end if;
                    end if;
                
                when COMPLETE =>
                    done <= '1';
                    
                when others =>
                    -- Should never reach here
                    state <= IDLE;
            end case;
        end if;
    end process;
    
    -- Next state logic
    process(state, start, aes_done, gcm_done, data_in_valid, aad_valid)
    begin
        next_state <= state;
        
        case state is
            when IDLE =>
                if start = '1' then
                    next_state <= INIT_AES;
                end if;
            
            when INIT_AES =>
                next_state <= GEN_HASHKEY;
            
            when GEN_HASHKEY =>
                if aes_done = '1' then
                    next_state <= PROCESS_AAD;
                end if;
            
            when PROCESS_AAD =>
                if gcm_done = '1' then
                    next_state <= PROCESS_DATA;
                end if;
            
            when PROCESS_DATA =>
                if gcm_done = '1' then
                    next_state <= FINALIZE;
                end if;
            
            when FINALIZE =>
                next_state <= COMPLETE;
            
            when COMPLETE =>
                next_state <= IDLE;
            
            when others =>
                next_state <= IDLE;
        end case;
    end process;

end Behavioral;

----------------------------------------------------------------------------------
-- QUIC Compression/Decompression Module
-- 
-- This module implements hardware-accelerated data compression/decompression
-- for QUIC protocol using a modified LZ77 algorithm.
--
-- Features:
-- - High-speed parallel compression/decompression
-- - Configurable dictionary size
-- - Optimized for QUIC packet payloads
----------------------------------------------------------------------------------
library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity quic_compression is
    Port (
        -- Clock and reset
        clk             : in  STD_LOGIC;
        rst             : in  STD_LOGIC;
        
        -- Control signals
        start           : in  STD_LOGIC;
        compress        : in  STD_LOGIC;  -- '1' for compress, '0' for decompress
        done            : out STD_LOGIC;
        
        -- Data interface for input
        data_in         : in  STD_LOGIC_VECTOR(63 downto 0);
        data_in_valid   : in  STD_LOGIC;
        data_in_last    : in  STD_LOGIC;  -- Indicates last data block
        data_in_ready   : out STD_LOGIC;
        
        -- Data interface for output
        data_out        : out STD_LOGIC_VECTOR(63 downto 0);
        data_out_valid  : out STD_LOGIC;
        data_out_last   : out STD_LOGIC;  -- Indicates last data block
        data_out_ready  : in  STD_LOGIC;
        
        -- Statistics
        compression_ratio : out STD_LOGIC_VECTOR(15 downto 0)  -- Fixed-point representation (8.8)
    );
end quic_compression;

architecture Behavioral of quic_compression is
    -- Constants
    constant DICT_SIZE      : integer := 4096;  -- Dictionary size in bytes
    constant MAX_MATCH_LEN  : integer := 258;   -- Maximum match length
    constant MIN_MATCH_LEN  : integer := 3;     -- Minimum match length
    
    -- Dictionary storage
    type dictionary_type is array (0 to DICT_SIZE-1) of STD_LOGIC_VECTOR(7 downto 0);
    signal dictionary : dictionary_type;
    
    -- Sliding window pointers
    signal dict_write_ptr : integer range 0 to DICT_SIZE-1;
    signal dict_read_ptr  : integer range 0 to DICT_SIZE-1;
    
    -- Internal buffers
    type buffer_type is array (0 to 1023) of STD_LOGIC_VECTOR(7 downto 0);
    signal input_buffer  : buffer_type;
    signal output_buffer : buffer_type;
    signal in_buf_wrptr  : integer range 0 to 1023;
    signal in_buf_rdptr  : integer range 0 to 1023;
    signal out_buf_wrptr : integer range 0 to 1023;
    signal out_buf_rdptr : integer range 0 to 1023;
    
    -- Compression metrics
    signal input_bytes  : unsigned(31 downto 0);
    signal output_bytes : unsigned(31 downto 0);
    
    -- State machine definition
    type state_type is (IDLE, LOAD_DATA, PROCESS_COMPRESSION, PROCESS_DECOMPRESSION, 
                         LOOKUP_MATCH, ENCODE_LITERAL, ENCODE_MATCH, 
                         OUTPUT_DATA, FINALIZE, COMPLETE);
    signal state, next_state : state_type;
    
    -- Match finding signals
    signal current_byte      : STD_LOGIC_VECTOR(7 downto 0);
    signal match_found       : STD_LOGIC;
    signal match_position    : integer range 0 to DICT_SIZE-1;
    signal match_length      : integer range 0 to MAX_MATCH_LEN;
    signal best_match_pos    : integer range 0 to DICT_SIZE-1;
    signal best_match_len    : integer range 0 to MAX_MATCH_LEN;
    
begin
    -- Main state machine process
    process(clk, rst)
    begin
        if rst = '1' then
            state <= IDLE;
            done <= '0';
            data_in_ready <= '0';
            data_out_valid <= '0';
            data_out_last <= '0';
            dict_write_ptr <= 0;
            dict_read_ptr <= 0;
            in_buf_wrptr <= 0;
            in_buf_rdptr <= 0;
            out_buf_wrptr <= 0;
            out_buf_rdptr <= 0;
            input_bytes <= (others => '0');
            output_bytes <= (others => '0');
            compression_ratio <= (others => '0');
        elsif rising_edge(clk) then
            state <= next_state;
            
            case state is
                when IDLE =>
                    done <= '0';
                    if start = '1' then
                        data_in_ready <= '1';
                        data_out_valid <= '0';
                        input_bytes <= (others => '0');
                        output_bytes <= (others => '0');
                    end if;
                
                when LOAD_DATA =>
                    if data_in_valid = '1' and data_in_ready = '1' then
                        -- Load 64-bit word into input buffer, byte by byte
                        for i in 0 to 7 loop
                            input_buffer(in_buf_wrptr) <= data_in((i*8+7) downto (i*8));
                            in_buf_wrptr <= (in_buf_wrptr + 1) mod 1024;
                        end loop;
                        input_bytes <= input_bytes + 8;
                        
                        if data_in_last = '1' then
                            -- Last data block received
                            data_in_ready <= '0';
                            if compress = '1' then
                                next_state <= PROCESS_COMPRESSION;
                            else
                                next_state <= PROCESS_DECOMPRESSION;
                            end if;
                        end if;
                    end if;
                
                when PROCESS_COMPRESSION =>
                    -- Main compression algorithm logic
                    if in_buf_rdptr /= in_buf_wrptr then
                        -- More data to compress
                        current_byte <= input_buffer(in_buf_rdptr);
                        next_state <= LOOKUP_MATCH;
                    else
                        -- Compression complete
                        next_state <= FINALIZE;
                    end if;
                
                when LOOKUP_MATCH =>
                    -- Search for the longest match in dictionary
                    match_found <= '0';
                    best_match_len <= 0;
                    
                    -- Simplified match finding logic (would be more complex in a real implementation)
                    for i in 0 to DICT_SIZE-1 loop
                        if dictionary(i) = current_byte then
                            -- Potential match found, check for length
                            -- This would be a parallel hardware matching circuit in reality
                            match_found <= '1';
                            match_position <= i;
                            match_length <= 1;
                            
                            -- Check for extending the match
                            -- In hardware, this would be unrolled with multiple comparators
                            if match_length > best_match_len then
                                best_match_pos <= match_position;
                                best_match_len <= match_length;
                            end if;
                        end if;
                    end loop;
                    
                    -- Decide what to encode
                    if match_found = '1' and best_match_len >= MIN_MATCH_LEN then
                        next_state <= ENCODE_MATCH;
                    else
                        next_state <= ENCODE_LITERAL;
                    end if;
                
                when ENCODE_LITERAL =>
                    -- Add current byte to dictionary
                    dictionary(dict_write_ptr) <= current_byte;
                    dict_write_ptr <= (dict_write_ptr + 1) mod DICT_SIZE;
                    
                    -- Output literal byte (in compressed format)
                    output_buffer(out_buf_wrptr) <= x"00"; -- Literal flag
                    out_buf_wrptr <= (out_buf_wrptr + 1) mod 1024;
                    output_buffer(out_buf_wrptr) <= current_byte;
                    out_buf_wrptr <= (out_buf_wrptr + 1) mod 1024;
                    output_bytes <= output_bytes + 2;
                    
                    -- Move to next input byte
                    in_buf_rdptr <= (in_buf_rdptr + 1) mod 1024;
                    next_state <= PROCESS_COMPRESSION;
                
                when ENCODE_MATCH =>
                    -- Output match (distance, length) in compressed format
                    output_buffer(out_buf_wrptr) <= x"01"; -- Match flag
                    out_buf_wrptr <= (out_buf_wrptr + 1) mod 1024;
                    
                    -- Encode distance (2 bytes)
                    output_buffer(out_buf_wrptr) <= STD_LOGIC_VECTOR(to_unsigned(best_match_pos, 8));
                    out_buf_wrptr <= (out_buf_wrptr + 1) mod 1024;
                    output_buffer(out_buf_wrptr) <= STD_LOGIC_VECTOR(to_unsigned(best_match_pos / 256, 8));
                    out_buf_wrptr <= (out_buf_wrptr + 1) mod 1024;
                    
                    -- Encode length (1 byte)
                    output_buffer(out_buf_wrptr) <= STD_LOGIC_VECTOR(to_unsigned(best_match_len, 8));
                    out_buf_wrptr <= (out_buf_wrptr + 1) mod 1024;
                    output_bytes <= output_bytes + 4;
                    
                    -- Add the matched bytes to dictionary and skip them in input
                    for i in 0 to best_match_len-1 loop
                        -- In hardware, this would be more parallelized
                        if in_buf_rdptr /= in_buf_wrptr then
                            dictionary(dict_write_ptr) <= input_buffer(in_buf_rdptr);
                            dict_write_ptr <= (dict_write_ptr + 1) mod DICT_SIZE;
                            in_buf_rdptr <= (in_buf_rdptr + 1) mod 1024;
                        end if;
                    end loop;
                    
                    next_state <= PROCESS_COMPRESSION;
                
                when PROCESS_DECOMPRESSION =>
                    -- Decompression algorithm logic (simplified)
                    if in_buf_rdptr /= in_buf_wrptr then
                        -- Check flag byte
                        if input_buffer(in_buf_rdptr) = x"00" then
                            -- Literal byte
                            in_buf_rdptr <= (in_buf_rdptr + 1) mod 1024;
                            if in_buf_rdptr /= in_buf_wrptr then
                                -- Get the literal byte
                                output_buffer(out_buf_wrptr) <= input_buffer(in_buf_rdptr);
                                out_buf_wrptr <= (out_buf_wrptr + 1) mod 1024;
                                dictionary(dict_write_ptr) <= input_buffer(in_buf_rdptr);
                                dict_write_ptr <= (dict_write_ptr + 1) mod DICT_SIZE;
                                output_bytes <= output_bytes + 1;
                                in_buf_rdptr <= (in_buf_rdptr + 1) mod 1024;
                            end if;
                        elsif input_buffer(in_buf_rdptr) = x"01" then
                            -- Match
                            in_buf_rdptr <= (in_buf_rdptr + 1) mod 1024;
                            -- Read distance and length (would need more sophisticated logic)
                            -- Simplified for illustration
                            match_position <= to_integer(unsigned(input_buffer(in_buf_rdptr)));
                            in_buf_rdptr <= (in_buf_rdptr + 2) mod 1024; -- Skip 2 bytes for distance
                            match_length <= to_integer(unsigned(input_buffer(in_buf_rdptr)));
                            in_buf_rdptr <= (in_buf_rdptr + 1) mod 1024;
                            
                            -- Copy bytes from dictionary to output
                            for i in 0 to match_length-1 loop
                                output_buffer(out_buf_wrptr) <= dictionary((match_position + i) mod DICT_SIZE);
                                out_buf_wrptr <= (out_buf_wrptr + 1) mod 1024;
                                dictionary(dict_write_ptr) <= dictionary((match_position + i) mod DICT_SIZE);
                                dict_write_ptr <= (dict_write_ptr + 1) mod DICT_SIZE;
                                output_bytes <= output_bytes + 1;
                            end loop;
                        end if;
                    else
                        next_state <= FINALIZE;
                    end if;
                
                when OUTPUT_DATA =>
                    -- Output data from the output buffer
                    if out_buf_rdptr /= out_buf_wrptr and data_out_ready = '1' then
                        -- Prepare 64-bit output word
                        for i in 0 to 7 loop
                            if out_buf_rdptr /= out_buf_wrptr then
                                data_out((i*8+7) downto (i*8)) <= output_buffer(out_buf_rdptr);
                                out_buf_rdptr <= (out_buf_rdptr + 1) mod 1024;
                            else
                                data_out((i*8+7) downto (i*8)) <= (others => '0');
                            end if;
                        end loop;
                        
                        data_out_valid <= '1';
                        if out_buf_rdptr = out_buf_wrptr then
                            data_out_last <= '1';
                            next_state <= COMPLETE;
                        else
                            data_out_last <= '0';
                        end if;
                    else
                        data_out_valid <= '0';
                        if out_buf_rdptr = out_buf_wrptr then
                            next_state <= COMPLETE;
                        end if;
                    end if;
                
                when FINALIZE =>
                    -- Calculate compression ratio
                    if output_bytes /= 0 then
                        -- Fixed-point calculation: (input_bytes / output_bytes) * 256
                        -- This would be implemented with a divider in hardware
                        compression_ratio <= STD_LOGIC_VECTOR(resize((input_bytes * 256) / output_bytes, 16));
                    else
                        compression_ratio <= (others => '0');
                    end if;
                    
                    next_state <= OUTPUT_DATA;
                
                when COMPLETE =>
                    done <= '1';
                    data_out_valid <= '0';
                    if start = '0' then
                        next_state <= IDLE;
                    end if;
                
                when others =>
                    next_state <= IDLE;
            end case;
        end if;
    end process;
    
    -- Next state logic
    process(state, start, data_in_valid, data_in_last, data_out_ready, in_buf_rdptr, in_buf_wrptr, out_buf_rdptr, out_buf_wrptr)
    begin
        next_state <= state;
        
        case state is
            when IDLE =>
                if start = '1' then
                    next_state <= LOAD_DATA;
                end if;
            
            when LOAD_DATA =>
                if data_in_last = '1' and data_in_valid = '1' then
                    if compress = '1' then
                        next_state <= PROCESS_COMPRESSION;
                    else
                        next_state <= PROCESS_DECOMPRESSION;
                    end if;
                end if;
            
            -- Other state transitions are handled in the main process
            
            when COMPLETE =>
                if start = '0' then
                    next_state <= IDLE;
                end if;
            
            when others =>
                -- Keep current state
        end case;
    end process;

end Behavioral;

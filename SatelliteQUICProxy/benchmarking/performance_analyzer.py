#!/usr/bin/env python3
"""
QUIC Proxy Performance Analyzer

This script benchmarks the performance of the QUIC proxy with and without
FPGA acceleration under simulated satellite conditions.
"""

import os
import sys
import time
import json
import signal
import socket
import argparse
import logging
import subprocess
import threading
import tempfile
import datetime
import csv
import random
import statistics
from pathlib import Path

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger('perf_analyzer')

class PerformanceTest:
    """Base class for performance tests"""
    
    def __init__(self, config_path, output_dir):
        """
        Initialize the performance test
        
        Args:
            config_path: Path to the JSON configuration file
            output_dir: Directory to store results
        """
        self.config_path = config_path
        self.output_dir = Path(output_dir)
        self.load_config()
        self.results = []
        self.running = False
        
        # Create output directory if it doesn't exist
        self.output_dir.mkdir(parents=True, exist_ok=True)
        
        # Register signal handlers
        signal.signal(signal.SIGINT, self.handle_signal)
        signal.signal(signal.SIGTERM, self.handle_signal)
    
    def load_config(self):
        """Load configuration from JSON file"""
        try:
            with open(self.config_path, 'r') as f:
                self.config = json.load(f)
                logger.info(f"Loaded configuration from {self.config_path}")
        except Exception as e:
            logger.error(f"Failed to load configuration: {e}")
            sys.exit(1)
    
    def prepare_test_data(self, size_kb):
        """Prepare test data of the specified size"""
        size_bytes = size_kb * 1024
        data_file = tempfile.NamedTemporaryFile(delete=False)
        
        try:
            # Generate random data
            with open(data_file.name, 'wb') as f:
                # We'll write in chunks to avoid memory issues with large files
                chunk_size = 10 * 1024 * 1024  # 10MB chunks
                remaining = size_bytes
                
                while remaining > 0:
                    write_size = min(chunk_size, remaining)
                    f.write(os.urandom(write_size))
                    remaining -= write_size
            
            logger.info(f"Created test data file: {data_file.name} ({size_kb} KB)")
            return data_file.name
        except Exception as e:
            logger.error(f"Failed to create test data: {e}")
            os.unlink(data_file.name)
            return None
    
    def run_transfer_test(self, data_file, proxy_enabled=True, fpga_enabled=True):
        """
        Run a data transfer test through the proxy
        
        Args:
            data_file: Path to the file containing test data
            proxy_enabled: Whether to route through the proxy
            fpga_enabled: Whether FPGA acceleration is enabled
        
        Returns:
            dict: Test results
        """
        proxy_addr = self.config['proxy_settings']['proxy_address']
        proxy_port = self.config['proxy_settings']['proxy_port']
        target_addr = self.config['proxy_settings']['target_address']
        target_port = self.config['proxy_settings']['target_port']
        
        # Get file size
        file_size = os.path.getsize(data_file)
        file_size_kb = file_size / 1024
        
        # Create a unique ID for this test
        test_id = f"test_{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}_{random.randint(1000, 9999)}"
        
        # Command for client and server components
        # In a real test, we'd have separate client and server programs
        # For this simulation, we'll use a dummy command to simulate the transfer
        
        start_time = time.time()
        
        try:
            if proxy_enabled:
                # Simulate transfer through proxy
                cmd = [
                    "bash", "-c",
                    f"cat {data_file} | "
                    f"nc {proxy_addr} {proxy_port} > /dev/null"
                ]
            else:
                # Simulate direct transfer
                cmd = [
                    "bash", "-c",
                    f"cat {data_file} | "
                    f"nc {target_addr} {target_port} > /dev/null"
                ]
            
            # Run the command and capture output
            logger.info(f"Starting transfer test: {' '.join(cmd)}")
            process = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            
            end_time = time.time()
            duration = end_time - start_time
            
            # Calculate throughput
            throughput_kbps = (file_size_kb * 8) / duration if duration > 0 else 0
            
            # Create result dictionary
            result = {
                'test_id': test_id,
                'timestamp': datetime.datetime.now().isoformat(),
                'file_size_kb': file_size_kb,
                'duration_sec': duration,
                'throughput_kbps': throughput_kbps,
                'proxy_enabled': proxy_enabled,
                'fpga_enabled': fpga_enabled,
                'success': process.returncode == 0,
                'error': None
            }
            
            if process.returncode != 0:
                result['error'] = f"Command failed: {process.stderr}"
                logger.error(f"Test failed: {process.stderr}")
            
            logger.info(f"Test completed: {result['success']}, "
                      f"Duration: {duration:.2f}s, "
                      f"Throughput: {throughput_kbps:.2f} kbps")
            
            return result
            
        except subprocess.TimeoutExpired:
            end_time = time.time()
            duration = end_time - start_time
            
            logger.error(f"Test timed out after {duration:.2f} seconds")
            
            return {
                'test_id': test_id,
                'timestamp': datetime.datetime.now().isoformat(),
                'file_size_kb': file_size_kb,
                'duration_sec': duration,
                'throughput_kbps': 0,
                'proxy_enabled': proxy_enabled,
                'fpga_enabled': fpga_enabled,
                'success': False,
                'error': "Timeout"
            }
            
        except Exception as e:
            end_time = time.time()
            duration = end_time - start_time
            
            logger.error(f"Test failed: {e}")
            
            return {
                'test_id': test_id,
                'timestamp': datetime.datetime.now().isoformat(),
                'file_size_kb': file_size_kb,
                'duration_sec': duration,
                'throughput_kbps': 0,
                'proxy_enabled': proxy_enabled,
                'fpga_enabled': fpga_enabled,
                'success': False,
                'error': str(e)
            }
    
    def save_results(self):
        """Save test results to a CSV file"""
        if not self.results:
            logger.warning("No results to save")
            return
        
        timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        output_file = self.output_dir / f"performance_results_{timestamp}.csv"
        
        try:
            with open(output_file, 'w', newline='') as f:
                writer = csv.DictWriter(f, fieldnames=self.results[0].keys())
                writer.writeheader()
                writer.writerows(self.results)
            
            logger.info(f"Results saved to {output_file}")
        except Exception as e:
            logger.error(f"Failed to save results: {e}")
    
    def analyze_results(self):
        """Analyze and print summary of test results"""
        if not self.results:
            logger.warning("No results to analyze")
            return
        
        # Group results by test configuration
        grouped_results = {}
        
        for result in self.results:
            if not result['success']:
                continue
                
            key = (result['file_size_kb'], result['proxy_enabled'], result['fpga_enabled'])
            if key not in grouped_results:
                grouped_results[key] = []
            
            grouped_results[key].append(result)
        
        # Print summary
        logger.info("\n===== Test Results Summary =====")
        
        for key, results in grouped_results.items():
            file_size_kb, proxy_enabled, fpga_enabled = key
            
            throughputs = [r['throughput_kbps'] for r in results]
            durations = [r['duration_sec'] for r in results]
            
            if not throughputs:
                continue
                
            avg_throughput = statistics.mean(throughputs)
            avg_duration = statistics.mean(durations)
            
            config_description = (
                f"File Size: {file_size_kb:.1f} KB, "
                f"Proxy: {'Enabled' if proxy_enabled else 'Disabled'}, "
                f"FPGA: {'Enabled' if fpga_enabled else 'Disabled'}"
            )
            
            logger.info(f"\n{config_description}")
            logger.info(f"Samples: {len(results)}")
            logger.info(f"Average Throughput: {avg_throughput:.2f} kbps")
            logger.info(f"Average Duration: {avg_duration:.2f} seconds")
            
            if len(throughputs) > 1:
                logger.info(f"Throughput Std Dev: {statistics.stdev(throughputs):.2f} kbps")
                logger.info(f"Duration Std Dev: {statistics.stdev(durations):.2f} seconds")
            
            logger.info(f"Min Throughput: {min(throughputs):.2f} kbps")
            logger.info(f"Max Throughput: {max(throughputs):.2f} kbps")
        
        logger.info("\n================================")
    
    def handle_signal(self, signum, frame):
        """Handle termination signals"""
        logger.info(f"Received signal {signum}, shutting down...")
        self.running = False
        self.save_results()
        self.analyze_results()
        sys.exit(0)
    
    def run_tests(self):
        """Run all configured performance tests"""
        self.running = True
        
        # Get test configuration
        test_duration_sec = self.config['test_settings']['test_duration_sec']
        test_data_sizes = self.config['test_settings']['test_data_sizes_kb']
        concurrent_connections = self.config['test_settings']['concurrent_connections']
        test_interval_sec = self.config['test_settings']['test_interval_sec']
        
        logger.info(f"Starting performance tests for {test_duration_sec} seconds")
        logger.info(f"Test data sizes: {test_data_sizes} KB")
        logger.info(f"Concurrent connections: {concurrent_connections}")
        logger.info(f"Test interval: {test_interval_sec} seconds")
        
        # Prepare test data files
        test_files = {}
        for size_kb in test_data_sizes:
            test_files[size_kb] = self.prepare_test_data(size_kb)
        
        # Run tests until time expires
        start_time = time.time()
        end_time = start_time + test_duration_sec
        
        try:
            # Test configurations - we'll test 3 modes:
            # 1. Direct connection (no proxy)
            # 2. Proxy without FPGA acceleration
            # 3. Proxy with FPGA acceleration
            
            test_configs = [
                {"proxy": False, "fpga": False},
                {"proxy": True, "fpga": False},
                {"proxy": True, "fpga": True}
            ]
            
            while time.time() < end_time and self.running:
                # Randomly select test data size
                size_kb = random.choice(test_data_sizes)
                data_file = test_files[size_kb]
                
                # Randomly select test configuration
                config = random.choice(test_configs)
                
                # Run the test
                result = self.run_transfer_test(
                    data_file, 
                    proxy_enabled=config["proxy"], 
                    fpga_enabled=config["fpga"]
                )
                
                # Store the result
                self.results.append(result)
                
                # Sleep for the test interval
                time.sleep(test_interval_sec)
        
        finally:
            # Clean up test data files
            for file_path in test_files.values():
                try:
                    os.unlink(file_path)
                except:
                    pass
            
            # Save and analyze results
            self.save_results()
            self.analyze_results()
            
            self.running = False

def main():
    parser = argparse.ArgumentParser(description='QUIC Proxy Performance Analyzer')
    parser.add_argument('-c', '--config', default='../simulation/config.json',
                       help='Path to configuration file (default: ../simulation/config.json)')
    parser.add_argument('-o', '--output', default='./results',
                       help='Directory for result files (default: ./results)')
    parser.add_argument('-v', '--verbose', action='store_true',
                       help='Enable verbose logging')
    args = parser.parse_args()
    
    # Set log level
    if args.verbose:
        logger.setLevel(logging.DEBUG)
    
    try:
        # Create and run the performance test
        tester = PerformanceTest(args.config, args.output)
        tester.run_tests()
    except KeyboardInterrupt:
        logger.info("Interrupted by user")
    except Exception as e:
        logger.error(f"Error in main loop: {e}")
        if args.verbose:
            import traceback
            traceback.print_exc()
        return 1

if __name__ == "__main__":
    sys.exit(main() or 0)

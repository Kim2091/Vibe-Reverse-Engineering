import sys
import re

def parse_log(filename):
    print(f"Parsing {filename}...")
    
    mat_starts = set()
    other_starts = set()
    
    try:
        with open(filename, 'r', encoding='utf-8', errors='ignore') as f:
            lines = f.readlines()
            
            for i, line in enumerate(lines):
                if "SetVSConstF" in line:
                    start_match = re.search(r"start=(\d+)", line)
                    if start_match:
                        start = int(start_match.group(1))
                        
                        # Check next line for count
                        if i + 1 < len(lines):
                            count_match = re.search(r"count=(\d+)", lines[i+1])
                            if count_match:
                                count = int(count_match.group(1))
                                if count == 4:
                                    mat_starts.add(start)
                                elif count > 1:
                                    other_starts.add((start, count))
                                    
    except Exception as e:
        print(f"Error reading file: {e}")
        
    print(f"Found matrix (count=4) writes at registers: {sorted(list(mat_starts))}")
    print(f"Found other multi-register writes at: {sorted(list(other_starts))}")

if __name__ == "__main__":
    parse_log(sys.argv[1])

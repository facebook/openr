# OpenR Roadmap

Here will be a high level overview of what we wish to do to Open/R in the up
comming halves.

## 2021 H1

### Planned Improvements

- Memory and CPU efficiency on route advertisement, computation, and programming
- Scaling an Open/R to carry 10k RIB size and 100k Adj-Rib size
- Secure thrift between KvStore instances

### Planned Features

- Area - Configure Open/R with multi-area setup by limiting the area topology
- FIB Ack - Re-distribute only programmed routes. More of Reliability
  enhancement
- Policy - Control inter-area route redistribution
- Route Origination - Native route origination via static config
- Segment routing support for multi-area setup

### Future Considerations

- Unequal Cost Multi-Path (UCMP) support in Open/R

### Deprecations

- Flooding Optimization (With current optimization worst case flooding remains
  the same)
- KvStore ZMQ interface
- Legacy route representation with metric vector
- Loop Free Alternate (LFA) feature in Open/R
- Ordered FIB programming

## 2021 H2

### Planned Improvements

- TBA

### Planned Features

- TBA

### Future Considerations

- TBA

### Deprecations

- TBA

## Best Effort Work

- Fix CI to be more reliable and pass - Issue #56
- Fix breeze thrift py3 building in Open Source Software - Issue #72
- Automate pushng Docker images on commits to master of GitHub

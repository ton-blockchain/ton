const API_URL = '/api';

// State management
let testsData = [];
let currentPage = 1;
const rowsPerPage = 15;
let selectedUser = 'all';

// Initialize the application
document.addEventListener('DOMContentLoaded', () => {
    loadTests();
    
    // Refresh button handler
    document.getElementById('refreshBtn').addEventListener('click', () => {
        loadTests();
    });

    const userFilter = document.getElementById('userFilter');
    if (userFilter) {
        userFilter.addEventListener('change', () => {
            selectedUser = userFilter.value;
            currentPage = 1;
            renderTable(currentPage);
        });
    }
});

// Load all test files from the API
async function loadTests() {
    const loadingEl = document.getElementById('loading');
    const errorEl = document.getElementById('error');
    const tableBody = document.getElementById('testsTableBody');
    
    loadingEl.style.display = 'block';
    errorEl.style.display = 'none';
    tableBody.innerHTML = '';
    
    try {
        const response = await fetch(`${API_URL}/tests`);
        
        if (!response.ok) {
            throw new Error(`HTTP error! status: ${response.status}`);
        }
        
        testsData = await response.json();
        
        loadingEl.style.display = 'none';

        updateUserFilterOptions();
        currentPage = 1;
        renderTable(currentPage);
    } catch (error) {
        console.error('Error loading tests:', error);
        loadingEl.style.display = 'none';
        errorEl.textContent = `Error loading test files: ${error.message}`;
        errorEl.style.display = 'block';
    }
}

// Render the table with test data
function renderTable(page = 1) {
    const tableBody = document.getElementById('testsTableBody');
    tableBody.innerHTML = '';

    const filteredIndices = getFilteredIndices();

    if (testsData.length === 0) {
        tableBody.innerHTML = '<tr><td colspan="9" style="text-align: center; padding: 30px; color: #6c757d;">No test files found in /tests directory</td></tr>';
        renderPagination(0);
        return;
    }

    if (filteredIndices.length === 0) {
        tableBody.innerHTML = '<tr><td colspan="9" style="text-align: center; padding: 30px; color: #6c757d;">No records for the selected user</td></tr>';
        renderPagination(0);
        return;
    }

    // Calculate pagination
    const startIndex = (page - 1) * rowsPerPage;
    const endIndex = startIndex + rowsPerPage;
    const paginatedIndices = filteredIndices.slice(startIndex, endIndex);

    paginatedIndices.forEach((index) => {
        const test = testsData[index];
        // Main row
        const row = document.createElement('tr');
        row.className = 'test-row';
        row.dataset.index = index;
        
        // Extract username from filename (e.g., "neodix" from "neodix-20251105065727.txt")
        const userName = extractUserName(test.fileName);
        
        // Generate the link with start and end parameters
        // Link to port 8000 on the same domain
        // If endTime is missing, use current timestamp
        const endTime = test.endTimeISO || new Date().toISOString();
        const linkUrl = test.startTimeISO 
            ? `${window.location.protocol}//${window.location.hostname}:8000/?start=${test.startTimeISO}&end=${endTime}`
            : '#';
        
        row.innerHTML = `
            <td>${escapeHtml(userName)}</td>
            <td>${escapeHtml(test.fileDateTime)}</td>
            <td class="description-cell" data-file-id="${escapeHtml(test.fileName.replace('.txt', ''))}" data-description="${escapeHtml(test.description)}">${escapeHtml(test.description)}</td>
            <td>${escapeHtml(test.nodes)}</td>
            <td>${escapeHtml(test.gitBranch)}</td>
            <td>${escapeHtml(test.duration || '')}</td>
            <td class="seconds-per-block-cell">${formatSecondsPerBlock(test.bpsMc, test.spbMc)}</td>
            <td><a href="${linkUrl}" target="_blank" class="external-link" onclick="event.stopPropagation()">view</a></td>
            <td class="actions-cell">
                <button class="delete-btn" onclick="event.stopPropagation(); deleteTest(${index})" title="Delete test">
                    <svg width="16" height="16" viewBox="0 0 16 16" fill="currentColor">
                        <path d="M5.5 5.5A.5.5 0 0 1 6 6v6a.5.5 0 0 1-1 0V6a.5.5 0 0 1 .5-.5zm2.5 0a.5.5 0 0 1 .5.5v6a.5.5 0 0 1-1 0V6a.5.5 0 0 1 .5-.5zm3 .5a.5.5 0 0 0-1 0v6a.5.5 0 0 0 1 0V6z"/>
                        <path fill-rule="evenodd" d="M14.5 3a1 1 0 0 1-1 1H13v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V4h-.5a1 1 0 0 1-1-1V2a1 1 0 0 1 1-1H6a1 1 0 0 1 1-1h2a1 1 0 0 1 1 1h3.5a1 1 0 0 1 1 1v1zM4.118 4 4 4.059V13a1 1 0 0 0 1 1h6a1 1 0 0 0 1-1V4.059L11.882 4H4.118zM2.5 3V2h11v1h-11z"/>
                    </svg>
                </button>
            </td>
        `;
        
        // Add click handler to expand/collapse
        row.addEventListener('click', (e) => {
            // Don't toggle if clicking on description cell
            if (!e.target.classList.contains('description-cell')) {
                toggleDetails(index);
            }
        });
        
        // Add double-click handler for description cell
        const descriptionCell = row.querySelector('.description-cell');
        descriptionCell.addEventListener('dblclick', (e) => {
            e.stopPropagation();
            makeDescriptionEditable(descriptionCell, index);
        });
        
        tableBody.appendChild(row);
        
        // Details row
        const detailsRow = document.createElement('tr');
        detailsRow.className = 'details-row';
        detailsRow.dataset.index = index;
        
        const detailsCell = document.createElement('td');
        detailsCell.className = 'details-cell';
        detailsCell.colSpan = 9;
        detailsCell.innerHTML = createDetailsContent(test);
        
        detailsRow.appendChild(detailsCell);
        tableBody.appendChild(detailsRow);
    });
    
    // Render pagination controls
    renderPagination(filteredIndices.length);
}

// Render pagination controls
function renderPagination(totalRows) {
    const totalPages = Math.ceil(totalRows / rowsPerPage);
    
    // Remove existing pagination if present
    let paginationContainer = document.getElementById('pagination');
    if (paginationContainer) {
        paginationContainer.remove();
    }
    
    // Only show pagination if there's more than one page
    if (totalPages <= 1) {
        return;
    }
    
    // Create pagination container
    paginationContainer = document.createElement('div');
    paginationContainer.id = 'pagination';
    paginationContainer.className = 'pagination';
    
    // Previous button
    const prevButton = document.createElement('button');
    prevButton.className = 'pagination-btn';
    prevButton.textContent = '← Previous';
    prevButton.disabled = currentPage === 1;
    prevButton.addEventListener('click', () => {
        if (currentPage > 1) {
            currentPage--;
            renderTable(currentPage);
        }
    });
    paginationContainer.appendChild(prevButton);
    
    // Page info
    const pageInfo = document.createElement('span');
    pageInfo.className = 'pagination-info';
    pageInfo.textContent = `Page ${currentPage} of ${totalPages}`;
    paginationContainer.appendChild(pageInfo);
    
    // Page number buttons (show current, previous, and next pages)
    const pageNumbers = document.createElement('div');
    pageNumbers.className = 'pagination-numbers';
    
    // Calculate which page numbers to show
    let startPage = Math.max(1, currentPage - 2);
    let endPage = Math.min(totalPages, currentPage + 2);
    
    // Adjust if we're near the beginning or end
    if (currentPage <= 3) {
        endPage = Math.min(5, totalPages);
    }
    if (currentPage >= totalPages - 2) {
        startPage = Math.max(1, totalPages - 4);
    }
    
    // First page button
    if (startPage > 1) {
        const firstBtn = createPageButton(1);
        pageNumbers.appendChild(firstBtn);
        if (startPage > 2) {
            const ellipsis = document.createElement('span');
            ellipsis.className = 'pagination-ellipsis';
            ellipsis.textContent = '...';
            pageNumbers.appendChild(ellipsis);
        }
    }
    
    // Page number buttons
    for (let i = startPage; i <= endPage; i++) {
        const pageBtn = createPageButton(i);
        pageNumbers.appendChild(pageBtn);
    }
    
    // Last page button
    if (endPage < totalPages) {
        if (endPage < totalPages - 1) {
            const ellipsis = document.createElement('span');
            ellipsis.className = 'pagination-ellipsis';
            ellipsis.textContent = '...';
            pageNumbers.appendChild(ellipsis);
        }
        const lastBtn = createPageButton(totalPages);
        pageNumbers.appendChild(lastBtn);
    }
    
    paginationContainer.appendChild(pageNumbers);
    
    // Next button
    const nextButton = document.createElement('button');
    nextButton.className = 'pagination-btn';
    nextButton.textContent = 'Next →';
    nextButton.disabled = currentPage === totalPages;
    nextButton.addEventListener('click', () => {
        if (currentPage < totalPages) {
            currentPage++;
            renderTable(currentPage);
        }
    });
    paginationContainer.appendChild(nextButton);
    
    // Insert pagination after the table
    const tableContainer = document.querySelector('.table-container');
    tableContainer.appendChild(paginationContainer);
}

// Create a page number button
function createPageButton(pageNum) {
    const button = document.createElement('button');
    button.className = 'pagination-number';
    if (pageNum === currentPage) {
        button.classList.add('active');
    }
    button.textContent = pageNum;
    button.addEventListener('click', () => {
        currentPage = pageNum;
        renderTable(currentPage);
    });
    return button;
}

// Toggle details view for a test
async function toggleDetails(index) {
    const row = document.querySelector(`tr.test-row[data-index="${index}"]`);
    const detailsRow = document.querySelector(`tr.details-row[data-index="${index}"]`);
    
    const isExpanded = row.classList.contains('expanded');
    
    if (isExpanded) {
        row.classList.remove('expanded');
        detailsRow.classList.remove('show');
    } else {
        row.classList.add('expanded');
        detailsRow.classList.add('show');
        
        // Load network quality if not already loaded
        const test = testsData[index];
        const qualitySection = detailsRow.querySelector('.network-quality-section');
        
        if (qualitySection && !qualitySection.dataset.loaded) {
            await loadNetworkQuality(test, qualitySection);
        }
        
        // Load network configuration if not already loaded
        const networkSection = detailsRow.querySelector('.network-config-section');
        
        if (networkSection && !networkSection.dataset.loaded) {
            await loadNetworkConfig(test, networkSection);
        }
        
        // Load gremlin logs if not already loaded
        const gremlinLogsSection = detailsRow.querySelector('.gremlin-logs-section');
        
        if (gremlinLogsSection && !gremlinLogsSection.dataset.loaded) {
            await loadGremlinLogs(test, gremlinLogsSection);
        }
    }
}

// Create the details content HTML
function createDetailsContent(test) {
    let html = '<div class="details-content">';
    
    // Test Information Section
    html += '<div class="details-section">';
    html += '<h3>Test Information</h3>';
    html += '<div class="info-grid">';
    html += `<div class="info-item">
        <div class="info-label">Start Date & Time</div>
        <div class="info-value">${escapeHtml(test.startDateTime)}</div>
    </div>`;
    html += `<div class="info-item">
        <div class="info-label">Git Branch</div>
        <div class="info-value">${escapeHtml(test.gitBranch)}</div>
    </div>`;
    html += `<div class="info-item">
        <div class="info-label">Git Commit ID</div>
        <div class="info-value commit-id">${escapeHtml(test.gitCommitId)}</div>
    </div>`;
    html += `<div class="info-item">
        <div class="info-label">Number of Nodes</div>
        <div class="info-value">${escapeHtml(test.nodes)}</div>
    </div>`;
    html += '</div>';
    html += '</div>';
    
    // Configuration Section
    if (test.configuration && Object.keys(test.configuration).length > 0) {
        html += '<div class="details-section">';
        html += '<h3>Configuration</h3>';
        html += '<div class="config-grid">';
        
        Object.entries(test.configuration).forEach(([key, value]) => {
            if (key !== 'configuration') {
                html += `<div class="config-item">
                    <span class="config-key">${escapeHtml(key)}:</span>
                    <span class="config-value">${escapeHtml(value)}</span>
                </div>`;
            }
        });
        
        html += '</div>';
        html += '</div>';
    }
    
    // Operations Timeline Section
    if (test.operations && test.operations.length > 0) {
        html += '<div class="details-section">';
        html += '<h3>Operations Timeline</h3>';
        html += '<div class="operations-timeline">';
        
        test.operations.forEach(op => {
            const opClass = op.operation.toLowerCase();
            html += `<div class="operation-item ${opClass}">
                <div class="operation-type">${escapeHtml(op.operation)}</div>
                <div class="operation-datetime">${escapeHtml(op.data.date_time || 'N/A')}</div>
            </div>`;
        });
        
        html += '</div>';
        html += '</div>';
    }
    
    // Network Quality Section (placeholder, will be loaded on demand)
    html += '<div class="details-section network-quality-section">';
    html += '<h3>Network Quality</h3>';
    html += '<div class="network-quality-content">';
    html += '<div class="loading-network">Loading network quality...</div>';
    html += '</div>';
    html += '</div>';
    
    // Network Configuration Section (placeholder, will be loaded on demand)
    html += '<div class="details-section network-config-section">';
    html += '<h3>Network Configuration</h3>';
    html += '<div class="network-config-content">';
    html += '<div class="loading-network">Loading network configuration...</div>';
    html += '</div>';
    html += '</div>';
    
    // Gremlin Logs Section (placeholder, will be loaded on demand)
    html += '<div class="details-section gremlin-logs-section">';
    html += '<h3>Gremlin Logs</h3>';
    html += '<div class="gremlin-logs-content">';
    html += '<div class="loading-network">Loading gremlin logs...</div>';
    html += '</div>';
    html += '</div>';
    
    html += '</div>';
    return html;
}

// Load network quality for a test
async function loadNetworkQuality(test, qualitySection) {
    const contentDiv = qualitySection.querySelector('.network-quality-content');
    
    try {
        // Extract the ID from the filename (e.g., "neodix-20251122054201" from "neodix-20251122054201.txt")
        const fileId = test.fileName.replace('.txt', '');
        
        const response = await fetch(`${API_URL}/network-quality/${fileId}`);
        
        if (!response.ok) {
            if (response.status === 404) {
                // File not found - keep section empty as per requirements
                contentDiv.innerHTML = '';
            } else {
                throw new Error(`HTTP error! status: ${response.status}`);
            }
            qualitySection.dataset.loaded = 'true';
            return;
        }
        
        const data = await response.json();
        
        // Display the network quality content in a preformatted block
        contentDiv.innerHTML = `<pre class="network-config-pre">${escapeHtml(data.content)}</pre>`;
        
        // Mark as loaded
        qualitySection.dataset.loaded = 'true';
    } catch (error) {
        console.error('Error loading network quality:', error);
        contentDiv.innerHTML = `<div class="network-config-error">Error loading network quality: ${escapeHtml(error.message)}</div>`;
    }
}

// Load network configuration for a test
async function loadNetworkConfig(test, networkSection) {
    const contentDiv = networkSection.querySelector('.network-config-content');
    
    try {
        // Extract the ID from the filename (e.g., "neodix-20251114092034" from "neodix-20251114092034.txt")
        const fileId = test.fileName.replace('.txt', '');
        
        const response = await fetch(`${API_URL}/network/${fileId}`);
        
        if (!response.ok) {
            if (response.status === 404) {
                contentDiv.innerHTML = '<div class="network-config-error">Network configuration file not found</div>';
            } else {
                throw new Error(`HTTP error! status: ${response.status}`);
            }
            return;
        }
        
        const data = await response.json();
        
        // Display the network configuration content in a preformatted block
        contentDiv.innerHTML = `<pre class="network-config-pre">${escapeHtml(data.content)}</pre>`;
        
        // Mark as loaded
        networkSection.dataset.loaded = 'true';
    } catch (error) {
        console.error('Error loading network configuration:', error);
        contentDiv.innerHTML = `<div class="network-config-error">Error loading network configuration: ${escapeHtml(error.message)}</div>`;
    }
}

// Load gremlin logs for a test
async function loadGremlinLogs(test, gremlinLogsSection) {
    const contentDiv = gremlinLogsSection.querySelector('.gremlin-logs-content');
    
    try {
        // Extract the ID from the filename (e.g., "neodix-20251130091704" from "neodix-20251130091704.txt")
        const fileId = test.fileName.replace('.txt', '');
        
        const response = await fetch(`${API_URL}/gremlin-logs/${fileId}`);
        
        if (!response.ok) {
            if (response.status === 404) {
                // File not found - keep section empty
                contentDiv.innerHTML = '';
            } else {
                throw new Error(`HTTP error! status: ${response.status}`);
            }
            gremlinLogsSection.dataset.loaded = 'true';
            return;
        }
        
        const data = await response.json();
        
        // Display the gremlin logs content in a preformatted block
        contentDiv.innerHTML = `<pre class="network-config-pre">${escapeHtml(data.content)}</pre>`;
        
        // Mark as loaded
        gremlinLogsSection.dataset.loaded = 'true';
    } catch (error) {
        console.error('Error loading gremlin logs:', error);
        contentDiv.innerHTML = `<div class="network-config-error">Error loading gremlin logs: ${escapeHtml(error.message)}</div>`;
    }
}

// Format seconds per block data for display
// Displays both bps mc and spb mc lines
function formatSecondsPerBlock(bpsMc, spbMc) {
    if (!bpsMc && !spbMc) return '';
    
    let html = '<div class="spb-data">';
    
    if (bpsMc) {
        html += `<div class="spb-line">${escapeHtml(bpsMc)}</div>`;
    }
    
    if (spbMc) {
        html += `<div class="spb-line">${escapeHtml(spbMc)}</div>`;
    }
    
    html += '</div>';
    return html;
}

// Extract username from filename (e.g., "neodix" from "neodix-20251105065727.txt")
function extractUserName(fileName) {
    if (!fileName) return '';
    
    // Remove .txt extension if present
    const nameWithoutExt = fileName.replace(/\.txt$/, '');
    
    // Extract everything before the first hyphen
    const hyphenIndex = nameWithoutExt.indexOf('-');
    if (hyphenIndex > 0) {
        return nameWithoutExt.substring(0, hyphenIndex);
    }
    
    // If no hyphen found, return the whole filename without extension
    return nameWithoutExt;
}

function getFilteredIndices() {
    if (selectedUser === 'all') {
        return testsData.map((_, index) => index);
    }

    return testsData.reduce((indices, test, index) => {
        if (extractUserName(test.fileName) === selectedUser) {
            indices.push(index);
        }
        return indices;
    }, []);
}

function updateUserFilterOptions() {
    const userFilter = document.getElementById('userFilter');
    if (!userFilter) return;

    const previousSelection = userFilter.value || selectedUser || 'all';
    const users = Array.from(new Set(testsData.map((test) => extractUserName(test.fileName))))
        .filter((user) => user)
        .sort((a, b) => a.localeCompare(b));

    userFilter.innerHTML = '';
    userFilter.appendChild(new Option('All users', 'all'));

    users.forEach((user) => {
        userFilter.appendChild(new Option(user, user));
    });

    if (users.includes(previousSelection)) {
        userFilter.value = previousSelection;
        selectedUser = previousSelection;
    } else {
        userFilter.value = 'all';
        selectedUser = 'all';
    }
}

// Delete a test with confirmation
async function deleteTest(index) {
    const test = testsData[index];
    const userName = extractUserName(test.fileName);
    // Extract the base filename without extension - this is what we'll use as the ID
    // For "20251105061044.txt" -> "20251105061044"
    // For "neodix-20251105065727.txt" -> "neodix-20251105065727"
    const fileId = test.fileName.replace(/\.txt$/, '');
    
    // Create confirmation dialog
    const confirmed = confirm(
        `Are you sure you want to delete this test?\n\n` +
        `User: ${userName}\n` +
        `Date: ${test.fileDateTime}\n` +
        `Description: ${test.description}\n\n` +
        `This will delete all associated files (*.txt, *.net, *.slow, *.slow.summary)`
    );
    
    if (!confirmed) {
        return;
    }
    
    try {
        const response = await fetch(`${API_URL}/tests/${fileId}`, {
            method: 'DELETE'
        });
        
        if (!response.ok) {
            const errorData = await response.json();
            throw new Error(errorData.error || `HTTP error! status: ${response.status}`);
        }
        
        const result = await response.json();
        
        // Show success message
        console.log('Deleted files:', result.deletedFiles);
        
        // Remove the test from local data
        testsData.splice(index, 1);
        
        // Adjust current page if needed
        const totalPages = Math.ceil(getFilteredIndices().length / rowsPerPage);
        if (currentPage > totalPages && totalPages > 0) {
            currentPage = totalPages;
        }
        
        // Re-render the table
        updateUserFilterOptions();
        renderTable(currentPage);
        
        // Show success notification
        showNotification('Test deleted successfully', 'success');
    } catch (error) {
        console.error('Error deleting test:', error);
        showNotification(`Error deleting test: ${error.message}`, 'error');
    }
}

// Show notification message
function showNotification(message, type = 'info') {
    // Remove existing notification if present
    const existingNotification = document.querySelector('.notification');
    if (existingNotification) {
        existingNotification.remove();
    }
    
    // Create notification element
    const notification = document.createElement('div');
    notification.className = `notification notification-${type}`;
    notification.textContent = message;
    
    // Add to page
    document.body.appendChild(notification);
    
    // Auto-remove after 3 seconds
    setTimeout(() => {
        notification.classList.add('fade-out');
        setTimeout(() => {
            notification.remove();
        }, 300);
    }, 3000);
}

// Make description cell editable
function makeDescriptionEditable(cell, index) {
    const currentDescription = cell.dataset.description;
    const fileId = cell.dataset.fileId;
    
    // Check if description is "No description" or empty
    if (currentDescription !== 'No description' && currentDescription.trim() !== '') {
        return; // Only allow editing if it's "No description" or empty
    }
    
    // Store original content
    const originalContent = cell.innerHTML;
    
    // Create input element
    const input = document.createElement('input');
    input.type = 'text';
    input.className = 'description-input';
    input.value = currentDescription === 'No description' ? '' : currentDescription;
    input.placeholder = 'Enter description...';
    
    // Replace cell content with input
    cell.innerHTML = '';
    cell.appendChild(input);
    input.focus();
    input.select();
    
    // Function to save the description
    const saveDescription = async () => {
        const newDescription = input.value.trim();
        
        // If empty, don't save
        if (!newDescription) {
            cell.innerHTML = originalContent;
            return;
        }
        
        // If unchanged, don't save
        if (newDescription === currentDescription) {
            cell.innerHTML = originalContent;
            return;
        }
        
        try {
            // Show loading state
            cell.innerHTML = '<span style="color: #6c757d; font-style: italic;">Saving...</span>';
            
            const response = await fetch(`${API_URL}/tests/${fileId}/description`, {
                method: 'PATCH',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify({ description: newDescription })
            });
            
            if (!response.ok) {
                const errorData = await response.json();
                throw new Error(errorData.error || `HTTP error! status: ${response.status}`);
            }
            
            // Update local data
            testsData[index].description = newDescription;
            
            // Update cell
            cell.dataset.description = newDescription;
            cell.textContent = newDescription;
            
            // Show success notification
            showNotification('Description updated successfully', 'success');
        } catch (error) {
            console.error('Error updating description:', error);
            cell.innerHTML = originalContent;
            showNotification(`Error updating description: ${error.message}`, 'error');
        }
    };
    
    // Function to cancel editing
    const cancelEdit = () => {
        cell.innerHTML = originalContent;
    };
    
    // Handle Enter key to save
    input.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
            e.preventDefault();
            saveDescription();
        } else if (e.key === 'Escape') {
            e.preventDefault();
            cancelEdit();
        }
    });
    
    // Handle blur (clicking outside) to save
    input.addEventListener('blur', () => {
        // Small delay to allow other events to process
        setTimeout(saveDescription, 100);
    });
    
    // Prevent row click from triggering
    input.addEventListener('click', (e) => {
        e.stopPropagation();
    });
}

// Escape HTML to prevent XSS
function escapeHtml(text) {
    if (!text) return '';
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}
